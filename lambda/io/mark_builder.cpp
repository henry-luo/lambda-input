// MarkBuilder - Fluent API for constructing Lambda document structures
//
// String Management Strategy (unified name/symbol system):
//   - createName(): Always pooled via NamePool (string interning)
//       Use for: map keys, element tags, attribute names - structural identifiers
//       pooling shares storage; cross-owner equality follows NameKey (D5.6.1)
//
//   - createString(): Never pooled (arena allocated)
//       Use for: user content, text data, string values - non-structural content
//       Fast allocation, no hash lookup overhead
//
//   - createSymbol(): Arena allocated; identity follows NameKey (S8.2.2v6)
//       Use for: symbol literals ('mySymbol), short enum-like values
//
// Memory Benefits:
//   - Structural names deduplicated across entire document hierarchy
//   - Parent NamePool inheritance (schemas share names with instances)
//   - Content strings remain fast with arena allocation

#include "mark_builder.hpp"
#include "../core/lambda-decimal.hpp"
#include "../core/data.hpp"
#include "../lambda.h"  // for it2l, it2s, it2b, it2i, it2d, etc.
#include "../core/mark_reader.hpp"  // for ArrayReader
#include "../core/lambda_typed.hpp"
#include "lib/str.h"
#include "lib/string.h"
#include "../input/input.hpp"
#include "../input/css/dom_node.hpp"      // for DomText, dom_text_to_string
#include "../input/css/dom_element.hpp"   // for DomElement, dom_element_to_element
#include "lib/mempool.h"
#include "lib/arena.h"
#include "lib/strbuf.h"
#include "lib/stringbuf.h"
#include "lib/arraylist.h"
#include "lib/hashmap.h"
#include "lib/re2_glue.hpp"
#include "lib/log.h"
#include "lib/memtrack.h"
#include <cstring>
#include <cassert>
#include <new>

// Forward declarations for internal functions in input.cpp
extern Element* input_create_element_internal(Input *input, const char* tag_name);

extern TypeMap EmptyMap;

static inline Item mark_item_from_bits(uint64_t bits) {
    // clang 14 rejects designated Item initializers in instantiated deep-copy
    // branches, so construct the same tagged word through its raw field.
    Item result = ItemNull;
    result.item = bits;
    return result;
}

//==============================================================================
// MarkBuilder Implementation
//==============================================================================

MarkBuilder::MarkBuilder(Input* input)
    : input_(input)
    , pool_(input->pool)
    , arena_(input->arena)
    , name_pool_(input->name_pool)
    , type_list_(input->type_list)
    , ui_mode_(input->ui_mode)
{
    assert(input != nullptr);
    assert(pool_ != nullptr);
    assert(arena_ != nullptr);
    assert(name_pool_ != nullptr);
    assert(type_list_ != nullptr);
}

MarkBuilder::~MarkBuilder() {
    // RAII cleanup - nothing to do since we don't own any resources
    // Arena-allocated data lives until Input's arena is reset/destroyed
}

//------------------------------------------------------------------------------
// Heap factory (audited boundary for `new MarkBuilder` / `delete builder`)
//------------------------------------------------------------------------------

MarkBuilder* mark_builder_create(Input* input) {
    if (!input) return nullptr;
    MarkBuilder* builder = (MarkBuilder*)mem_alloc(sizeof(MarkBuilder), MEM_CAT_EVAL);
    if (!builder) return nullptr;
    new (builder) MarkBuilder(input); // NEW_DELETE_OK: single audited construction boundary for MarkBuilder.
    return builder;
}

void mark_builder_destroy(MarkBuilder* builder) {
    if (!builder) return;
    builder->~MarkBuilder(); // NEW_DELETE_OK: paired with mark_builder_create.
    mem_free(builder);
}

//------------------------------------------------------------------------------
// Name Creation Methods (always use name_pool)
//------------------------------------------------------------------------------

String* MarkBuilder::createName(const char* name) {
    if (!name) return nullptr;
    return createName(name, strlen(name));
}

String* MarkBuilder::createName(const char* name, size_t len) {
    if (!name) return nullptr;
    // JSON and similar formats can carry an empty object key; preserve it as a structural name.
    return name_pool_create_len(name_pool_, name, len);
}

String* MarkBuilder::createNameFromStrView(StrView name) {
    if (!name.str) return nullptr;
    return name_pool_create_strview(name_pool_, name);
}

//------------------------------------------------------------------------------
// Symbol Creation Methods (arena-allocated Symbol structs)
//------------------------------------------------------------------------------

Symbol* MarkBuilder::createSymbol(const char* symbol) {
    if (!symbol) return nullptr;
    return createSymbol(symbol, strlen(symbol));
}

Symbol* MarkBuilder::createSymbol(const char* symbol, size_t len) {
    if (!symbol || len == 0) return nullptr;
    Symbol* sym = (Symbol*)arena_alloc(arena_, sizeof(Symbol) + len + 1);
    sym->len = len;
    sym->kind = SYMBOL_LAMBDA_NAME;
    sym->ns = nullptr;
    str_copy(sym->chars, len + 1, symbol, len);
    return sym;
}

Symbol* MarkBuilder::createSymbolFromStrView(StrView symbol) {
    return createSymbol(symbol.str, symbol.length);
}

//------------------------------------------------------------------------------
// String Creation Methods (arena allocation, no pooling)
//------------------------------------------------------------------------------

String* MarkBuilder::createString(const char* str) {
    if (!str) return nullptr;
    return createString(str, strlen(str));
}

String* MarkBuilder::createString(const char* str, size_t len) {
    if (!str) return nullptr;

    // Empty strings are values in Phase 3, so content producers must not collapse them to null.
    return string_from_strview_arena(strview_init(str, len), arena_);
}

Binary* MarkBuilder::createBinary(const void* bytes, size_t len) {
    if ((!bytes && len != 0) || len > UINT32_MAX ||
        len > SIZE_MAX - sizeof(Binary)) return nullptr;

    // Arena objects have no individual finalizer, so their immutable byte
    // payload must remain in the arena instead of retaining external storage.
    Binary* binary = (Binary*)arena_alloc(arena_, sizeof(Binary) + len);
    if (!binary) return nullptr;
    binary->len = (uint32_t)len;
    binary->is_ascii = (len == 0 || str_is_ascii((const char*)bytes, len)) ? 1 : 0;
    binary->flags = BINARY_FLAG_INLINE;
    binary->reserved = 0;
    binary->storage = nullptr;
    binary->offset = 0;
    if (len != 0) memcpy(binary->inline_bytes, bytes, len);
    return binary;
}

String* MarkBuilder::createStringFromBuf(StringBuf* sb) {
    if (!sb) return nullptr;
    return createString(sb->str->chars, sb->length);
}

String* MarkBuilder::createDomTextString(const char* str, size_t len) {
    if (!str) return nullptr;

    // Allocate [DomText][String header][chars...\0] as one contiguous block.
    DomText* dt = DomText::create_in(arena_, len);
    if (!dt) return nullptr;
    String* s = dom_text_to_string(dt);
    s->flags = 0;
    s->is_ascii = str_is_ascii(str, len) ? 1 : 0;
    str_copy(s->chars, len + 1, str, len);
    return s;
}

String* MarkBuilder::createDomTextStringFromBuf(StringBuf* sb) {
    if (!sb || sb->length == 0) return nullptr;
    return createDomTextString(sb->str->chars, sb->length);
}

String* MarkBuilder::emptyString() {
    return nullptr;
}

//------------------------------------------------------------------------------
// Item Creation Helpers
//------------------------------------------------------------------------------

Item MarkBuilder::createNameItem(const char* name) {
    Symbol* sym = createSymbol(name);  // create proper Symbol for correct memory layout
    if (!sym) return createNull();
    return (Item){.item = y2it(sym)};
}

Item MarkBuilder::createSymbolItem(const char* symbol) {
    Symbol* sym = createSymbol(symbol);
    // Empty symbol maps to null (createSymbol returns nullptr for empty)
    if (!sym) return createNull();
    return (Item){.item = y2it(sym)};
}

Item MarkBuilder::createStringItem(const char* str) {
    String* s = createString(str);
    // createString returns nullptr only for null input; empty strings are real values.
    if (!s) return createNull();
    return (Item){.item = s2it(s)};
}

Item MarkBuilder::createStringItem(const char* str, size_t len) {
    String* s = createString(str, len);
    // createString returns nullptr only for null input; empty strings are real values.
    if (!s) return createNull();
    return (Item){.item = s2it(s)};
}

//------------------------------------------------------------------------------
// Builder Creation Methods
//------------------------------------------------------------------------------

ElementBuilder MarkBuilder::element(const char* tag_name) {
    return ElementBuilder(this, tag_name);
}

MapBuilder MarkBuilder::map() {
    return MapBuilder(this);
}

ArrayBuilder MarkBuilder::array() {
    return ArrayBuilder(this);
}

//------------------------------------------------------------------------------
// Direct Item Creation
//------------------------------------------------------------------------------

Item MarkBuilder::createElement(const char* tag_name) {
    return element(tag_name).final();
}

lam::ItemOf<LMD_TYPE_ELEMENT> MarkBuilder::createElementTyped(const char* tag_name) {
    return lam::require<LMD_TYPE_ELEMENT>(createElement(tag_name));
}

Item MarkBuilder::createMap() {
    return map().final();
}

lam::ItemOf<LMD_TYPE_MAP> MarkBuilder::createMapTyped() {
    return lam::require<LMD_TYPE_MAP>(createMap());
}

Item MarkBuilder::createArray() {
    return array().final();
}

lam::ItemOf<LMD_TYPE_ARRAY> MarkBuilder::createArrayTyped() {
    return lam::require<LMD_TYPE_ARRAY>(createArray());
}

Item MarkBuilder::createNumericArray(ArrayNumElemType type, int rank, const int64_t* dimensions,
        const void* samples) {
    if (rank < 1 || rank > LAMBDA_ARRAY_NUM_MAX_NDIM || !dimensions ||
            (type & 15) || (unsigned)type > ELEM_UINT8_CLAMPED) return ItemNull;
    size_t element_size = ELEM_TYPE_SIZE[type >> 4];
    int64_t length = 1;
    for (int axis = 0; axis < rank; axis++) {
        if (dimensions[axis] < 0 || (dimensions[axis] && length > INT64_MAX / dimensions[axis]))
            return ItemNull;
        length *= dimensions[axis];
    }
    if (!element_size || (uint64_t)length > (SIZE_MAX - sizeof(ArrayNum)) / element_size) return ItemNull;
    auto* array = (ArrayNum*)arena_calloc(arena_, sizeof(ArrayNum) + (size_t)length * element_size);
    auto* shape = (ArrayNumShape*)arena_calloc(arena_, sizeof(ArrayNumShape) + 2 * (size_t)rank * sizeof(int64_t));
    if (!array || !shape) return ItemNull;
    array->type_id = LMD_TYPE_ARRAY_NUM; array->set_elem_type(type);
    array->is_static = 1; array->cow_state = COW_STATE_SHARED;
    array->capacity = array->length = length; array->data = (char*)array + sizeof(ArrayNum);
    array->is_ndim = 1; array->extra = (int64_t)(uintptr_t)shape;
    shape->ndim = rank; shape->is_c_contig = 1; shape->is_f_contig = rank == 1;
    int64_t stride = 1;
    for (int axis = rank - 1; axis >= 0; axis--) {
        array_num_shape_dims(shape)[axis] = dimensions[axis];
        array_num_shape_strides(shape)[axis] = stride;
        if (dimensions[axis] && stride > INT64_MAX / dimensions[axis]) return ItemNull;
        stride *= dimensions[axis];
    }
    if (samples && length) memcpy(array->data, samples, (size_t)length * element_size);
    return mark_item_from_bits((uint64_t)(uintptr_t)array);
}

Item MarkBuilder::createInt(int64_t value) {
    Item item = {.item = i2it(value)};
    return item;
}

Item MarkBuilder::createLong(int64_t value) {
    // allocate long from arena
    int64_t* long_ptr = (int64_t*)arena_alloc(arena_, sizeof(int64_t));
    *long_ptr = value;
    Item item = {.item = l2it(long_ptr)};
    return item;
}

Item MarkBuilder::createUInt64(uint64_t value) {
    // Static Mark scalars must share the Input arena lifetime just like int64.
    uint64_t* uint64_ptr = (uint64_t*)arena_alloc(arena_, sizeof(uint64_t));
    if (!uint64_ptr) return ItemError;
    *uint64_ptr = value;
    return {.item = u2it(uint64_ptr)};
}

Item MarkBuilder::createFloat(double value) {
    // allocate double from arena
    double* float_ptr = (double*)arena_alloc(arena_, sizeof(double));
    *float_ptr = value;
    Item item = lambda_float_ptr_to_item(float_ptr);
    return item;
}

Item MarkBuilder::createDateTime(DateTime value) {
    if (DATETIME_IS_ERROR(value)) return ItemError;
    // Static parser output is owned by the Input arena; push_k would instead
    // introduce a GC lifetime into an otherwise arena-owned Mark graph.
    DateTime* datetime_ptr = (DateTime*)arena_alloc(arena_, sizeof(DateTime));
    if (!datetime_ptr) return ItemError;
    *datetime_ptr = value;
    return {.item = k2it(datetime_ptr)};
}

Item MarkBuilder::createBool(bool value) {
    return {.item = b2it(value)};
}

Item MarkBuilder::createNull() {
    return ItemNull;
}

Item MarkBuilder::createRange(int64_t start, int64_t end) {
    // Allocate Range from arena
    Range* range = (Range*)arena_alloc(arena_, sizeof(Range));
    if (!range) return createNull();

    range->type_id = LMD_TYPE_RANGE;
    range->flags = CONTAINER_FLAG_IMMORTAL;
    range->start = start;
    range->end = end;
    range->length = (end >= start) ? (end - start + 1) : 0;
    range->is_char = false;
    return {.range = range};
}

static bool mark_type_is_declared_nominal(const Type* type) {
    return lambda_type_is_concrete_attr_shape(type) && type->is_nominal &&
        ((const TypeMap*)type)->is_trusted_contract;
}

// type descriptors must have the full layout that readers and copies dispatch to.
static size_t mark_type_descriptor_size(const Type* source) {
    size_t bytes = sizeof(Type);
    if (source->type_id == LMD_TYPE_TYPE) {
        const size_t sizes[] = {sizeof(TypeType), sizeof(TypeUnary), sizeof(TypeBinary), sizeof(TypePattern),
            sizeof(TypeConstrained), sizeof(TypeRange), sizeof(TypeParam), sizeof(TypeBinder),
            sizeof(TypeBoundRef), sizeof(TypeGrammar), sizeof(TypeGrammarRule)};
        if (source->kind >= sizeof(sizes) / sizeof(sizes[0])) {
            log_error("dom-contract-copy: unsupported type kind %u", source->kind);
            return 0;
        }
        bytes = sizes[source->kind];
    } else if (mark_type_is_declared_nominal(source)) bytes = sizeof(TypeObject);
    else if (source->type_id == LMD_TYPE_MAP) bytes = sizeof(TypeMap);
    else if (source->type_id == LMD_TYPE_ELEMENT) bytes = sizeof(TypeElmt);
    else if (source->type_id == LMD_TYPE_ARRAY) bytes = sizeof(TypeArray);
    else if (source->type_id == LMD_TYPE_FUNC) bytes = sizeof(TypeFunc);
    else if (source->is_literal || source->is_const) {
        switch (source->type_id) {
        case LMD_TYPE_BOOL: bytes = sizeof(TypeConst); break;
        case LMD_TYPE_INT: case LMD_TYPE_INT64: bytes = sizeof(TypeInt64); break;
        case LMD_TYPE_UINT64: bytes = sizeof(TypeUint64); break;
        case LMD_TYPE_FLOAT: bytes = sizeof(TypeFloat); break;
        case LMD_TYPE_COMPLEX: bytes = sizeof(TypeComplex); break;
        case LMD_TYPE_NUM_SIZED: bytes = sizeof(TypeNumSized); break;
        case LMD_TYPE_DTIME: bytes = sizeof(TypeDateTime); break;
        case LMD_TYPE_STRING: case LMD_TYPE_SYMBOL: bytes = sizeof(TypeString); break;
        case LMD_TYPE_BINARY: bytes = sizeof(TypeBinaryConst); break;
        case LMD_TYPE_DECIMAL: bytes = sizeof(TypeDecimal); break;
        default: break;
        }
    }
    return bytes;
}

Item MarkBuilder::createType(TypeId type_id, bool is_literal, bool is_const) {
    Type header = {};
    header.type_id = type_id;
    header.is_literal = is_literal;
    header.is_const = is_const;
    size_t bytes = mark_type_descriptor_size(&header);
    Type* type = bytes ? (Type*)arena_calloc(arena_, bytes) : nullptr;
    if (!type) return {.item = ITEM_UNDEFINED};
    *type = header;

    return {.type = type};
}

Item MarkBuilder::createMetaType(TypeId type_id) {
    Type* sub_type = this->createType(type_id, true, true).type;
    if (sub_type) {
        TypeType *new_type = (TypeType*)this->createType(LMD_TYPE_TYPE, true, true).type;
        new_type->type = sub_type;
        return {.type = new_type};
    } else {
        log_debug("createMetaType: failed to create sub_type for type_id=%d", type_id);
    }
    return {.item = ITEM_UNDEFINED};
}

//------------------------------------------------------------------------------
// Internal Helpers
//------------------------------------------------------------------------------

void MarkBuilder::putToElement(lam::GcPtr<Element> elmt, String* key, Item value) {
    elmt_put_tree(elmt.get(), key, value, input_);
}

void MarkBuilder::putToMap(lam::GcPtr<Map> map, String* key, Item value) {
    map_put(map.get(), key, value, input_);
}

//==============================================================================
// ElementBuilder Implementation
//==============================================================================

ElementBuilder::ElementBuilder(MarkBuilder* builder, const char* tag_name)
    : builder_(builder)
    , tag_name_(builder->createName(tag_name))  // element names are structural identifiers - always pooled
    , elmt_(nullptr)
{
    Input* input = builder_->input();
    Element* element = nullptr;

    if (builder->dom_occurrence_mode()) {
        // dom occurrences embed the element in its native node storage.
        DomElement* dom = DomElement::create_in(input->arena);
        if (!dom) return;
        element = dom_element_to_element(dom);
        element->type_id = LMD_TYPE_ELEMENT;
        element->type = &EmptyElmt;
    } else {
        element = elmt_arena(input->arena);  // Use arena allocation for MarkBuilder
    }

    // D3.4.3: start on the tag's root in the Input's transition tree, so
    // elements with one tag and attribute sequence share one TypeElmt
    TypeElmt* root = element ? elmt_tree_root(input, tag_name_, NULL) : NULL;
    if (root) {
        element->type = root;
        elmt_ = element;
    } else if (element) {
        TypeElmt *element_type = (TypeElmt*)alloc_type(input->pool, LMD_TYPE_ELEMENT, sizeof(TypeElmt));
        if (element_type) {
            element->type = element_type;
            arraylist_append(input->type_list, element_type);
            element_type->type_index = input->type_list->length - 1;
            element_type->is_private_clone = true;
            // initialize with no attributes

            // set element name (use name pool for structural identifier)
            String* name_str = builder->createName(tag_name);
            if (name_str) {
                element_type->name.str = name_str->chars;
                element_type->name.length = name_str->len;
                element_type->name_id = name_ref_id(name_str);
            }
            elmt_ = (element);
        }
    }
}

ElementBuilder::~ElementBuilder() {
    // children_ is pool-allocated, no cleanup needed
    // attributes_ is pool-allocated, no cleanup needed
}

//------------------------------------------------------------------------------
// Attribute Setters
//------------------------------------------------------------------------------

ElementBuilder& ElementBuilder::attr(const char* key, Item value) {
    if (!key) return *this;
    // use elmt_put to add the attribute to the element
    String* key_str = builder_->createName(key);  // attribute names are structural identifiers - always pooled
    builder_->putToElement(lam::gc_borrow(elmt_), key_str, value);
    return *this;
}

ElementBuilder& ElementBuilder::attr(Target* ns, const char* key, Item value) {
    if (!ns) return attr(key, value);
    if (!key || !elmt_) return *this;
    elmt_put_ns(elmt_, builder_->createName(key), ns, value,
        builder_->input()->pool);
    return *this;
}

ElementBuilder& ElementBuilder::namespace_tag(Target* ns) {
    if (!elmt_ || !ns) return *this;
    // tree roots are shared; namespace metadata belongs to this occurrence.
    TypeElmt* type = elmt_clone_type_for_mutation(elmt_,
        builder_->input()->pool);
    if (type) type->ns = ns;
    return *this;
}

ElementBuilder& ElementBuilder::attr(const char* key, const char* value) {
    return attr(key, builder_->createStringItem(value));  // note: value content uses createString (NOT pooled)
}

ElementBuilder& ElementBuilder::attr(const char* key, int64_t value) {
    return attr(key, builder_->createInt(value));
}

ElementBuilder& ElementBuilder::attr(const char* key, int32_t value) {
    return attr(key, builder_->createInt(value));
}

ElementBuilder& ElementBuilder::attr(const char* key, uint64_t value) {
    return attr(key, builder_->createUInt64(value));
}

ElementBuilder& ElementBuilder::attr(const char* key, double value) {
    return attr(key, builder_->createFloat(value));
}

ElementBuilder& ElementBuilder::attr(const char* key, bool value) {
    return attr(key, builder_->createBool(value));
}

// String* key overloads
ElementBuilder& ElementBuilder::attr(String* key, Item value) {
    builder_->putToElement(lam::gc_borrow(elmt_), key, value);
    return *this;
}

ElementBuilder& ElementBuilder::attr(String* key, const char* value) {
    return attr(key, builder_->createStringItem(value));
}

ElementBuilder& ElementBuilder::attr(String* key, int64_t value) {
    return attr(key, builder_->createInt(value));
}

ElementBuilder& ElementBuilder::attr(String* key, int32_t value) {
    return attr(key, builder_->createInt(value));
}

ElementBuilder& ElementBuilder::attr(String* key, uint64_t value) {
    return attr(key, builder_->createUInt64(value));
}

ElementBuilder& ElementBuilder::attr(String* key, double value) {
    return attr(key, builder_->createFloat(value));
}

ElementBuilder& ElementBuilder::attr(String* key, bool value) {
    return attr(key, builder_->createBool(value));
}

//------------------------------------------------------------------------------
// Child Management
//------------------------------------------------------------------------------

ElementBuilder& ElementBuilder::child(Item item) {
    // S2.6.4: strings merge in content; arrays retain argument boundaries.
    list_push_with_owner((List*)elmt_, item, builder_->pool(), builder_->arena(),
        builder_->ui_mode() ? builder_->input() : nullptr);
    return *this;
}

ElementBuilder& ElementBuilder::text(const char* text) {
    if (text) {
        child(builder_->createStringItem(text));  // text content is user data - NOT pooled
    }
    return *this;
}

ElementBuilder& ElementBuilder::text(const char* text, size_t len) {
    if (text && len > 0) {
        child(builder_->createStringItem(text, len));  // text content is user data - NOT pooled
    }
    return *this;
}

ElementBuilder& ElementBuilder::children(std::initializer_list<Item> items) {
    for (const Item& item : items) {
        child(item);
    }
    return *this;
}

//------------------------------------------------------------------------------
Item ElementBuilder::final() {
    // nothing to finalize: an element's type came from the transition tree, or
    // is its own private type (D3.4.3)
    return (Item){.element = elmt_};
}

lam::ItemOf<LMD_TYPE_ELEMENT> ElementBuilder::finalTyped() {
    return lam::require<LMD_TYPE_ELEMENT>(final());
}

//==============================================================================
// MapBuilder Implementation
//==============================================================================

MapBuilder::MapBuilder(MarkBuilder* builder)
    : builder_(builder)
    , map_(nullptr)
    , map_type_(nullptr)
{
    // allocate map structure from arena (fast sequential allocation)
    // Note: map_put() uses pool for internal data buffers (needs pool_free for resizing)
    map_ = (Map*)arena_calloc(builder_->arena(), sizeof(Map));
    map_->type_id = LMD_TYPE_MAP;
    map_->is_immortal = 1;
    // D3.4.7: only an explicitly JS-bound Input supplies a branded root.
    map_->type = builder_->input()->branded_shape_transition_root
        ? builder_->input()->branded_shape_transition_root : &EmptyMap;
    map_->data = nullptr;
    map_->data_cap = 0;
}

MapBuilder::~MapBuilder() {
    // map_ is arena-allocated, no cleanup needed
}

MapBuilder& MapBuilder::put(const char* key, Item value) {
    // key could be null for nested map
    String* key_str = key ? builder_->createName(key) : nullptr;  // map keys are structural identifiers - always pooled
    builder_->putToMap(lam::gc_borrow(map_), key_str, value);

    // cache the type for convenience
    if (!map_type_) { map_type_ = (TypeMap*)map_->type; }
    return *this;
}

MapBuilder& MapBuilder::put(String* key, Item value) {
    if (!key) return *this;

    // use the existing string directly (caller responsible for pooling decision)
    builder_->putToMap(lam::gc_borrow(map_), key, value);

    // cache the type for convenience
    if (!map_type_) {
        map_type_ = (TypeMap*)map_->type;
    }

    return *this;
}

MapBuilder& MapBuilder::put(Target* ns, const char* local_name, Item value) {
    if (!ns) return put(local_name, value);
    map_put_ns(map_, builder_->createName(local_name), ns, value, builder_->input());
    map_type_ = (TypeMap*)map_->type;
    return *this;
}

MapBuilder& MapBuilder::put(const char* key, const char* value) {
    return put(key, builder_->createStringItem(value));
}

MapBuilder& MapBuilder::put(const char* key, int32_t value) {
    return put(key, {.item = i2it(value)});
}

MapBuilder& MapBuilder::put(const char* key, int64_t value) {
    return put(key, builder_->createLong(value));
}

MapBuilder& MapBuilder::put(const char* key, uint64_t value) {
    return put(key, builder_->createUInt64(value));
}

MapBuilder& MapBuilder::put(const char* key, double value) {
    return put(key, builder_->createFloat(value));
}

MapBuilder& MapBuilder::put(const char* key, bool value) {
    return put(key, builder_->createBool(value));
}

MapBuilder& MapBuilder::putNull(const char* key) {
    return put(key, ItemNull);
}

// String* key overloads
MapBuilder& MapBuilder::put(String* key, const char* value) {
    return put(key, builder_->createStringItem(value));
}

MapBuilder& MapBuilder::put(String* key, int64_t value) {
    return put(key, builder_->createInt(value));
}

MapBuilder& MapBuilder::put(String* key, uint64_t value) {
    return put(key, builder_->createUInt64(value));
}

MapBuilder& MapBuilder::put(String* key, double value) {
    return put(key, builder_->createFloat(value));
}

MapBuilder& MapBuilder::put(String* key, bool value) {
    return put(key, builder_->createBool(value));
}

Item MapBuilder::final() {
    // map_put already shared the type through the transition tree (D3.4.3)
    return (Item){.map = map_};
}

lam::ItemOf<LMD_TYPE_MAP> MapBuilder::finalTyped() {
    return lam::require<LMD_TYPE_MAP>(final());
}

//==============================================================================
// ArrayBuilder Implementation
//==============================================================================

ArrayBuilder::ArrayBuilder(MarkBuilder* builder)
    : builder_(builder)
    , array_(nullptr)
{
    // allocate Array from arena for MarkBuilder
    array_ = array_arena(builder_->arena());
}

ArrayBuilder::~ArrayBuilder() {
    // array_ is arena-allocated, no cleanup needed
}

ArrayBuilder& ArrayBuilder::append(Item item) {
    if (array_) {
        array_append(array_, item, builder_->pool(), builder_->arena());
    }
    return *this;
}

ArrayBuilder& ArrayBuilder::append(const char* str) {
    return append(builder_->createStringItem(str));
}

ArrayBuilder& ArrayBuilder::append(int64_t value) {
    return append(builder_->createInt(value));
}

ArrayBuilder& ArrayBuilder::append(int32_t value) {
    return append(builder_->createInt(value));
}

ArrayBuilder& ArrayBuilder::append(uint64_t value) {
    return append(builder_->createUInt64(value));
}

ArrayBuilder& ArrayBuilder::append(double value) {
    return append(builder_->createFloat(value));
}

ArrayBuilder& ArrayBuilder::append(bool value) {
    return append(builder_->createBool(value));
}

ArrayBuilder& ArrayBuilder::appendItems(std::initializer_list<Item> items) {
    for (const Item& item : items) {
        append(item);
    }
    return *this;
}

Item ArrayBuilder::final() {
    // Array is already built - just wrap it in an Item
    return (Item){.array = array_};
}

lam::ItemOf<LMD_TYPE_ARRAY> ArrayBuilder::finalTyped() {
    return lam::require<LMD_TYPE_ARRAY>(final());
}

// ============================================================================
// Deep Copy Implementation (MarkBuilder)
// ============================================================================

/**
 * Check if a pointer is owned by this Input's arena chain
 * Traverses parent Input chain to check all arenas
 */
bool MarkBuilder::is_pointer_in_arena_chain(const void* ptr) const {
    if (!ptr) return false;

    // Traverse the Input parent chain
    Input* current = input_;
    while (current) {
        if (arena_owns(current->arena, ptr)) {
            return true;
        }
        current = current->parent;
    }

    return false;
}

/**
 * Check if an Item's data is in this Input's arena chain
 * Returns true if all pointer data is owned by an arena in the chain
 */
bool MarkBuilder::is_in_arena(Item item) const {
    TypeId type_id = get_type_id(item);
    switch (type_id) {
    // Inline types - always safe to reuse
    case LMD_TYPE_NULL:  case LMD_TYPE_BOOL:  case LMD_TYPE_INT:  case LMD_TYPE_ERROR:
        return true;

    case LMD_TYPE_FLOAT:
        // Self-tagged floats do not carry an arena pointer, so ownership is implicit.
        if ((item.item & ITEM_DBL_MASK) || item.double_ptr <= 1) return true;
        return is_pointer_in_arena_chain((void*)item.double_ptr);

    // Pointer types - check arena ownership
    case LMD_TYPE_INT64:  case LMD_TYPE_UINT64:  case LMD_TYPE_DECIMAL:
    case LMD_TYPE_STRING:  case LMD_TYPE_BINARY:  case LMD_TYPE_DTIME:
        return is_pointer_in_arena_chain((void*)item.string_ptr);

    case LMD_TYPE_SYMBOL: {
        Symbol* sym = item.get_safe_symbol();
        if (!sym) return true;

        // Check if in NamePool chain by chars (Symbol is no longer String)
        StrView sv = {.str = sym->chars, .length = sym->len};
        String* pooled = name_pool_lookup_strview(name_pool_, sv);
        if (!symbol_lambda_namespace(sym) && pooled && pooled->len == sym->len &&
                memcmp(pooled->chars, sym->chars, sym->len) == 0) return true;

        // Check arena ownership
        return is_pointer_in_arena_chain(sym);
    }

    // Container types - check container and all contents
    case LMD_TYPE_RANGE:  case LMD_TYPE_TYPE:
    case LMD_TYPE_ARRAY_NUM:
        return is_pointer_in_arena_chain(item.array);

    case LMD_TYPE_ARRAY: {
        List* list = item.array;
        if (!list) return true;  // Null list

        // Phase 5a: For lists, check BOTH struct ownership AND content ownership
        // Even if List struct is in our arena, it might contain external values
        for (int i = 0; i < list->length; i++) {
            if (!is_in_arena(list->items[i])) return false;  // External item found
        }

        // All items in arena - now check if List struct itself is in arena
        return is_pointer_in_arena_chain(list);
    }

    case LMD_TYPE_MAP: {
        Map* map = item.map;
        if (!map || !map->type || !map->data) return true;  // Null/empty map

        // Phase 5a: For maps, we need to check BOTH struct ownership AND content ownership
        // Even if the Map struct is in our arena, it might contain external values
        TypeMap* map_type = (TypeMap*)map->type;
        if (!map_type->shape) return true;  // No fields

        MapReader reader(map);
        lam::ShapeRef field = lam::shape_borrow(map_type->shape);
        while (field) {
            if (field->name && field->name->str) {
                Item field_item = map_shape_field_to_item(map->data, field.get());
                if (!is_in_arena(field_item)) return false;  // External value found
            }
            field = lam::shape_next(map_type, field);
        }

        // All fields are in arena - now check if Map struct itself is in arena
        return is_pointer_in_arena_chain(map);
    }

    case LMD_TYPE_ELEMENT: {
        Element* elem = item.element;
        if (!elem || !elem->type) return true;  // Null/empty element

        // for elements, check BOTH struct ownership AND content ownership
        TypeElmt* elem_type = (TypeElmt*)elem->type;

        // Check all attributes
        if (elem_type->length > 0) {
            lam::ShapeRef attr = lam::shape_borrow(elem_type->shape);
            while (attr) {
                if (attr->name) {
                    Item attr_item = map_shape_field_to_item(elem->data, attr.get());
                    if (!is_in_arena(attr_item)) return false;  // External attribute found
                }
                attr = lam::shape_next(elem_type, attr);
            }
        }

        // Check all children
        for (int i = 0; i < elem->length; i++) {
            if (!is_in_arena(elem->items[i])) return false;  // External child found
        }

        // All content in arena - now check if Element struct itself is in arena
        return is_pointer_in_arena_chain(elem);
    }

    default:
        // Unknown types - assume external
        return false;
    }
}

/**
 * Deep copy with smart ownership checking
 * Copies external data, or owned data when a distinct occurrence is required.
 */
Item MarkBuilder::deep_copy(Item item, bool force) {
    // Quick check: if item contains no pointers (null, bool, int), just return it
    TypeId type_id = get_type_id(item);
    if (type_id <= LMD_TYPE_INT) {
        return item;  // Inline types - always safe
    }

    // Optimization: if data already in our arena chain, return as-is
    if (!force && is_in_arena(item)) {
        return item;
    }

    // copy external data or explicitly materialize another owned occurrence
    return deep_copy_internal(item);
}

Item MarkBuilder::deep_copy_for_view(Item item) {
    return ui_mode_ ? deep_copy_for_dom(item) : deep_copy(item);
}

Item MarkBuilder::deep_copy_for_dom(Item item) {
    copying_view_ = true;
    dom_contract_copy_failed_ = false;
    Item copied = deep_copy_internal(item);
    copying_view_ = false;
    if (dom_contract_copies_) hashmap_free(dom_contract_copies_);
    dom_contract_copies_ = nullptr;
    return dom_contract_copy_failed_ ? ItemError : copied;
}

Item MarkBuilder::copy_ui_content(Item item) {
    preserving_owned_content_ = true;
    Item copied = deep_copy(item);
    preserving_owned_content_ = false;
    return copied;
}

static Target* copy_name_namespace(Pool* owner, Target* ns) {
    return ns ? namespace_target(owner, ns->original, ns->name_prefix, ns->name_separator) : nullptr;
}

struct DomContractCopy {
    const Type* source;
    Type* copy;
};
static uint64_t dom_contract_hash(const void* value, uint64_t, uint64_t) {
    return hashmap_xxhash3(&((const DomContractCopy*)value)->source, sizeof(Type*), 0, 0);
}
static int dom_contract_compare(const void* a, const void* b, void*) {
    const Type* x = ((const DomContractCopy*)a)->source;
    const Type* y = ((const DomContractCopy*)b)->source;
    return x == y ? 0 : (uintptr_t)x < (uintptr_t)y ? -1 : 1;
}

static bool dom_contract_is_permanent(const Type* type) {
    if (type->type_id < LMD_TYPE_COUNT &&
            (type == type_info[type->type_id].type || type == type_info[type->type_id].lit_type)) return true;
    // these contracts carry semantics by identity even when they share a TypeId.
    const Type* permanent[] = {&TYPE_FLOAT64, &TYPE_INTEGER, &TYPE_INTEGER_VALUE, &TYPE_NUMBER,
        &TYPE_NONE, &TYPE_OBJECT, &TYPE_LIST, &TYPE_DATE, &TYPE_TIME, &TYPE_ANY_NO_ERROR,
        &TYPE_ANY_NO_NULL, &TYPE_ANY_NO_ERROR_OR_NULL, &TYPE_I8, &TYPE_I16, &TYPE_I32,
        &TYPE_U8, &TYPE_U16, &TYPE_U32, &TYPE_F16, &TYPE_F32, &CONST_BOOL, &CONST_INT,
        &CONST_FLOAT, &CONST_STRING, &LIT_NULL, &LIT_BOOL, &LIT_BOOL_TRUE, &LIT_BOOL_FALSE,
        &LIT_INT, &LIT_INT64, &LIT_FLOAT, &LIT_COMPLEX, &LIT_DECIMAL, &LIT_STRING,
        &LIT_DTIME, &LIT_NUM_SIZED, &LIT_UINT64, &LIT_TYPE};
    for (const Type* item : permanent) if (type == item) return true;
    return false;
}

bool MarkBuilder::copy_dom_field(ShapeEntry* target, const ShapeEntry* source) {
    if (source->name) {
        String* owned = createName(source->name->str, source->name->length);
        StrView* name = (StrView*)pool_alloc(pool_, sizeof(StrView));
        if (!owned || !name) return false;
        *name = {owned->chars, owned->len};
        target->name = name;
        target->name_id = name_ref_id(owned);
    }
    target->ns = copy_name_namespace(pool_, source->ns);
    Type* contract = copy_dom_contract(source->type);
    if (!contract) return false;
    // the cached lane must point into the copied graph as well (D3.4.6/D5.5.3).
    shape_entry_set_type(target, contract);
    return true;
}

Type* MarkBuilder::copy_dom_contract(const Type* source) {
    Type* copied = copy_dom_contract_inner(source);
    if (source && !copied) dom_contract_copy_failed_ = true;
    return copied;
}

TypeParam* MarkBuilder::copy_dom_parameter(const TypeParam* source) {
    if (!source) return nullptr;
    DomContractCopy key = {source, nullptr};
    const DomContractCopy* found = (const DomContractCopy*)hashmap_get(dom_contract_copies_, &key);
    if (found) return (TypeParam*)found->copy;
    TypeParam* copied = (TypeParam*)pool_alloc(pool_, sizeof(TypeParam));
    if (!copied) { dom_contract_copy_failed_ = true; return nullptr; }
    *copied = *source;
    key.copy = copied;
    hashmap_set(dom_contract_copies_, &key);
    if (hashmap_oom(dom_contract_copies_)) { dom_contract_copy_failed_ = true; return nullptr; }
    copied->full_type = copy_dom_contract(source->full_type);
    copied->contract_type = copy_dom_contract(source->contract_type);
    copied->binder = (TypeBinder*)copy_dom_contract(source->binder);
    copied->next = copy_dom_parameter(source->next);
    return copied;
}

static void dom_contract_regex_release(void* value) {
    lam::re2_glue_release((re2::RE2*)value);
}
static re2::RE2* dom_contract_regex_copy(Pool* pool, const re2::RE2* source) {
    if (!source) return nullptr;
    re2::RE2* copied = lam::re2_glue_compile(source->pattern().data(), source->pattern().size(),
        source->options(), "dom-contract-copy");
    if (copied && !pool_add_cleanup(pool, dom_contract_regex_release, copied)) {
        lam::re2_glue_release(copied);
        return nullptr;
    }
    return copied;
}

Type* MarkBuilder::copy_dom_contract_inner(const Type* source) {
    if (!source || dom_contract_is_permanent(source)) return (Type*)source;
    if (!dom_contract_copies_) dom_contract_copies_ = hashmap_new(sizeof(DomContractCopy), 16,
        0, 0, dom_contract_hash, dom_contract_compare, nullptr, nullptr);
    if (!dom_contract_copies_) return nullptr;
    DomContractCopy key = {source, nullptr};
    const DomContractCopy* found = (const DomContractCopy*)hashmap_get(dom_contract_copies_, &key);
    if (found) return found->copy;
    size_t bytes = mark_type_descriptor_size(source);
    if (!bytes) return nullptr;
    Type* copy = (Type*)pool_alloc(pool_, bytes);
    if (!copy) return nullptr;
    memcpy(copy, source, bytes);
    // publish before following edges so recursive contracts and aliases stay shared.
    key.copy = copy;
    hashmap_set(dom_contract_copies_, &key);
    if (hashmap_oom(dom_contract_copies_)) return nullptr;
    bool ok = true;
    auto edge = [&](Type*& pointer) {
        if (!pointer) return;
        pointer = copy_dom_contract(pointer);
        if (!pointer) ok = false;
    };
    if (source->type_id == LMD_TYPE_TYPE) {
        switch (source->kind) {
        case TYPE_KIND_SIMPLE: edge(((TypeType*)copy)->type); break;
        case TYPE_KIND_UNARY: edge(((TypeUnary*)copy)->operand); break;
        case TYPE_KIND_BINARY: edge(((TypeBinary*)copy)->left); edge(((TypeBinary*)copy)->right); break;
        case TYPE_KIND_CONSTRAINED: edge(((TypeConstrained*)copy)->base); break;
        case TYPE_KIND_PARAM: {
            TypeParam* parameter = (TypeParam*)copy;
            edge(parameter->full_type); edge(parameter->contract_type);
            parameter->binder = (TypeBinder*)copy_dom_contract(parameter->binder);
            parameter->next = copy_dom_parameter(parameter->next);
            break;
        }
        case TYPE_KIND_PATTERN: {
            TypePattern* pattern = (TypePattern*)copy;
            const TypePattern* original = (const TypePattern*)source;
            pattern->source = original->source ? createString(original->source->chars, original->source->len) : nullptr;
            pattern->regex_source = original->regex_source ? createString(original->regex_source->chars, original->regex_source->len) : nullptr;
            pattern->re2 = dom_contract_regex_copy(pool_, original->re2);
            pattern->re2_unanchored = dom_contract_regex_copy(pool_, original->re2_unanchored);
            if ((original->source && !pattern->source) || (original->regex_source && !pattern->regex_source) ||
                    (original->re2 && !pattern->re2) || (original->re2_unanchored && !pattern->re2_unanchored)) return nullptr;
            break;
        }
        case TYPE_KIND_GRAMMAR: {
            TypeGrammar* grammar = (TypeGrammar*)copy;
            edge(grammar->type);
            if (grammar->name) grammar->name = createString(grammar->name->chars, grammar->name->len);
            if (grammar->rule_count) {
                grammar->rules = (GrammarRule**)pool_alloc(pool_, sizeof(GrammarRule*) * grammar->rule_count);
                if (!grammar->rules) return nullptr;
                memcpy(grammar->rules, ((const TypeGrammar*)source)->rules, sizeof(GrammarRule*) * grammar->rule_count);
            }
            break;
        }
        case TYPE_KIND_GRAMMAR_RULE: {
            TypeGrammarRule* rule = (TypeGrammarRule*)copy;
            edge(rule->type);
            rule->grammar = (TypeGrammar*)copy_dom_contract(rule->grammar);
            if (rule->name) rule->name = createString(rule->name->chars, rule->name->len);
            break;
        }
        case TYPE_KIND_BINDER: {
            TypeBinder* binder = (TypeBinder*)copy;
            edge(binder->bound);
            if (binder->parameter_name) binder->parameter_name = createString(binder->parameter_name->chars, binder->parameter_name->len);
            if (binder->name) {
                Name* name = (Name*)pool_alloc(pool_, sizeof(Name));
                if (!name) return nullptr;
                *name = {createName(binder->name->name->chars, binder->name->name->len), copy_name_namespace(pool_, binder->name->ns)};
                binder->name = name;
            }
            break;
        }
        case TYPE_KIND_BOUND_REF: edge(((TypeBoundRef*)copy)->bound); break;
        case TYPE_KIND_RANGE: {
            TypeRange* range = (TypeRange*)copy;
            range->start = deep_copy_internal(range->start); range->end = deep_copy_internal(range->end);
            break;
        }
        default: break;
        }
    } else if (source->type_id == LMD_TYPE_MAP || source->type_id == LMD_TYPE_ELEMENT) {
        TypeMap* map = (TypeMap*)copy;
        map->field_index = nullptr; map->field_count = map->field_capacity = 0;
        map->transitions = nullptr; map->retype_plans = nullptr; map->js_proto_entry_cache = nullptr;
        map->is_private_clone = true; map->is_transition_shared_shape = map->is_shared_constructor_shape = false;
        map->slot_entries = nullptr; map->slot_count = 0;
        map->struct_name = map->struct_name ? pool_strdup(pool_, map->struct_name) : nullptr;
        map->shape = clone_shape_entries(type_alloc_of_pool(pool_), (const TypeMap*)source, &map->last);
        if (((const TypeMap*)source)->shape && !map->shape) return nullptr;
        ShapeEntry* field = map->shape;
        FOR_EACH_MAP_FIELD((const TypeMap*)source, original) {
            if (!copy_dom_field(field, original)) return nullptr;
            field = typemap_next_field(map, field);
        }
        map->verified_contract = (TypeMap*)copy_dom_contract(map->verified_contract);
        if (source->type_id == LMD_TYPE_ELEMENT || mark_type_is_declared_nominal(source)) {
            TypeElmt* element = (TypeElmt*)copy;
            String* name = createName(element->name.str, element->name.length);
            if (!name) return nullptr;
            element->name = {name->chars, name->len};
            element->ns = copy_name_namespace(pool_, element->ns);
            element->content_list = (TypeList*)copy_dom_contract(element->content_list);
        }
        if (mark_type_is_declared_nominal(source)) {
            TypeObject* object = (TypeObject*)copy;
            String* name = createName(object->type_name.str, object->type_name.length);
            if (!name) return nullptr;
            object->type_name = {name->chars, name->len};
            object->base = (TypeObject*)copy_dom_contract(object->base);
        }
        // nominal records and executable metadata retain their host's rooted identity (D5.5.2/S2.1.4).
        typemap_hash_build(map, pool_);
    } else if (source->type_id == LMD_TYPE_ARRAY) {
        TypeArray* array = (TypeArray*)copy;
        edge(array->nested);
        if (array->item_patterns && array->length > 0) {
            array->item_patterns = (Item*)pool_alloc(pool_, sizeof(Item) * array->length);
            array->item_is_type_pattern = (uint8_t*)pool_alloc(pool_, array->length);
            if (!array->item_patterns || !array->item_is_type_pattern) return nullptr;
            const TypeArray* old = (const TypeArray*)source;
            for (int64_t i = 0; i < array->length; i++) {
                array->item_patterns[i] = deep_copy_internal(old->item_patterns[i]);
                array->item_is_type_pattern[i] = old->item_is_type_pattern ? old->item_is_type_pattern[i] : 0;
            }
        }
    } else if (source->type_id == LMD_TYPE_FUNC) {
        TypeFunc* function = (TypeFunc*)copy;
        function->param = copy_dom_parameter(function->param);
        edge(function->returned); edge(function->inferred_return);
        edge(function->return_contract); edge(function->error_type);
        if (function->binder_count) {
            function->binders = (TypeBinder**)pool_alloc(pool_, sizeof(TypeBinder*) * function->binder_count);
            if (!function->binders) return nullptr;
            const TypeFunc* original = (const TypeFunc*)source;
            for (uint16_t i = 0; i < function->binder_count; i++)
                function->binders[i] = (TypeBinder*)copy_dom_contract(original->binders[i]);
        }
    } else if (source->is_literal || source->is_const) {
        if (source->type_id == LMD_TYPE_STRING || source->type_id == LMD_TYPE_SYMBOL) {
            TypeString* literal = (TypeString*)copy;
            if (literal->string) literal->string = createString(literal->string->chars, literal->string->len);
        } else if (source->type_id == LMD_TYPE_BINARY) {
            TypeBinaryConst* literal = (TypeBinaryConst*)copy;
            if (literal->binary) literal->binary = createBinary(binary_data(literal->binary), binary_length(literal->binary));
        } else if (source->type_id == LMD_TYPE_DECIMAL) {
            TypeDecimal* literal = (TypeDecimal*)copy;
            Item value = deep_copy_internal({.item = c2it(literal->decimal)});
            literal->decimal = value.get_decimal();
        }
    }
    return ok ? copy : nullptr;
}

template<TypeId Tag>
Item MarkBuilder::deep_copy_typed(lam::ItemOf<Tag> typed) {
    Item item = typed.raw();

    if constexpr (Tag == LMD_TYPE_NULL || Tag == LMD_TYPE_BOOL || Tag == LMD_TYPE_INT) {
        return item;
    } else if constexpr (Tag == LMD_TYPE_INT64) {
        return createLong(typed.value());
    } else if constexpr (Tag == LMD_TYPE_UINT64) {
        // uint64 is pointer-backed, so decode its payload instead of using the inline-value API.
        return createUInt64(typed.raw().get_uint64());
    } else if constexpr (Tag == LMD_TYPE_FLOAT) {
        // Float Items may be self-tagged inline values; never assume pointer storage.
        return createFloat(typed.value());
    } else if constexpr (Tag == LMD_TYPE_SYMBOL) {
        Symbol* sym = typed.ptr();
        if (!sym) return createNull();
        Symbol* copied_sym = createSymbol(sym->chars, sym->len);
        if (copied_sym) copied_sym->ns = copy_name_namespace(pool_, symbol_lambda_namespace(sym));
        return mark_item_from_bits(y2it(copied_sym));
    } else if constexpr (Tag == LMD_TYPE_STRING) {
        String* str = typed.ptr();
        if (!str) return createNull();
        return createStringItem(str->chars, str->len);
    } else if constexpr (Tag == LMD_TYPE_BINARY) {
        Binary* bin = typed.ptr();
        if (!bin) return createNull();
        Binary* copied = createBinary(binary_data(bin), binary_length(bin));
        if (!copied) return createNull();
        return mark_item_from_bits(x2it(copied));
    } else if constexpr (Tag == LMD_TYPE_DTIME) {
        DateTime* dt = typed.ptr();
        return dt ? createDateTime(*dt) : ItemNull;
    } else if constexpr (Tag == LMD_TYPE_DECIMAL) {
        // Use centralized decimal_deep_copy function
        return decimal_deep_copy(item, arena_);
    } else if constexpr (Tag == LMD_TYPE_RANGE) {
        Range* src_range = typed.ptr();
        Item copied = createRange(src_range->start, src_range->end);
        if (get_type_id(copied) == LMD_TYPE_RANGE) copied.range->is_char = src_range->is_char;
        return copied;
    } else if constexpr (Tag == LMD_TYPE_ARRAY_NUM) {
        ArrayNum* arr = typed.ptr();
        size_t elem_size = ELEM_TYPE_SIZE[arr->get_elem_type() >> 4];
        int64_t dimensions[LAMBDA_ARRAY_NUM_MAX_NDIM], strides[LAMBDA_ARRAY_NUM_MAX_NDIM];
        int rank = array_num_get_shape_strides(arr, dimensions, strides);
        Item copied = createNumericArray(arr->get_elem_type(), rank, dimensions);
        if (get_type_id(copied) != LMD_TYPE_ARRAY_NUM || copied.array_num->length != arr->length) return ItemNull;
        ArrayNum* new_arr = copied.array_num;
        // materialize owned C-order storage; copying a view must retain neither its base nor its strides.
        for (int64_t i=0;i<arr->length;i++) {
            int64_t offset=array_num_element_offset(arr,i);
            memcpy((char*)new_arr->data+(size_t)i*elem_size,(char*)arr->data+offset*(int64_t)elem_size,elem_size);
        }
        return copied;
    } else if constexpr (Tag == LMD_TYPE_ARRAY) {
        Array* arr = typed.ptr();
        int64_t length = arr->length;       // arr->length is int64_t — do not truncate to int
        int64_t capacity = arr->capacity;
        (void)capacity;
        uint8_t src_flags = arr->flags;
        ArrayBuilder arr_builder = array();
        ArrayReader reader(lam::gc_borrow(arr));
        for (int64_t i = 0; i < length; i++) {
            Item child = reader.get(i).item();
            Item copied_child = deep_copy_internal(child);
            arr_builder.append(copied_child);
        }
        Item result = arr_builder.final();
        // the kind bit survives a copy: a list copies to a list (S2.5.1v2)
        if (result.array) {
            result.array->flags |= (src_flags & 0x02); // is_spreadable bit
        }
        return result;
    } else if constexpr (Tag == LMD_TYPE_MAP) {
        Map* src_map = typed.ptr();
        TypeMap* map_type = (TypeMap*)src_map->type;
        (void)map_type;
        MapBuilder map_builder = map();
        MapReader reader(lam::gc_borrow(src_map));
        MapReader::EntryIterator iter = reader.entries();
        const char* key;  ItemReader value; Target* ns;
        while (iter.next(&key, &value, &ns)) {
            Item field_item = value.item();
            Item copied_field = deep_copy_internal(field_item);
            if (key) {
                map_builder.put(copy_name_namespace(pool_, ns), key, copied_field);
            } else {
                map_builder.put(key, copied_field);
            }
        }
        return map_builder.final();
    } else if constexpr (Tag == LMD_TYPE_ELEMENT) {
        log_enter();
        Element* elem = typed.ptr();
        ElementReader reader(lam::gc_borrow(elem));

        // Get tag name via reader (returns const char*)
        const char* tag_name = reader.tagName();
        ElementBuilder elem_builder = element(tag_name);
        elem_builder.namespace_tag(copy_name_namespace(pool_, reader.namespaceTarget()));

        // Copy attributes using ElementReader
        // ElementReader.get_attr() handles proper Item reconstruction from stored data
        TypeElmt* elem_type = (TypeElmt*)elem->type;
        TypeElmt* view_type = nullptr;
        ShapeEntry* view_attr = nullptr;
        if (copying_view_) {
            // D5.5.3 copies values without erasing nominal, qualified, or declared field contracts.
            Element* occurrence = elem_builder.final().element;
            if (!occurrence) return ItemNull;
            TypeElmt* allocated_type = (TypeElmt*)occurrence->type;
            occurrence->type = elem_type;
            view_type = elmt_clone_type_for_mutation(occurrence, pool_, true);
            if (!view_type) return ItemNull;
            view_type->ns = copy_name_namespace(pool_, elem_type->ns);
            view_type->struct_name = elem_type->struct_name ? pool_strdup(pool_, elem_type->struct_name) : nullptr;
            view_type->content_list = (TypeList*)copy_dom_contract(elem_type->content_list);
            if (elem_type->content_list && !view_type->content_list) return ItemNull;
            view_type->name = allocated_type->name;
            view_type->name_id = allocated_type->name_id;
            // runtime element fields can have no reserve metadata; the shape owns their footprint.
            occurrence->data_cap = elem->data_cap > elem_type->byte_size
                ? elem->data_cap : elem_type->byte_size;
            occurrence->data = occurrence->data_cap > 0
                ? pool_calloc(pool_, (size_t)occurrence->data_cap) : nullptr;
            if (occurrence->data_cap > 0 && !occurrence->data) return ItemNull;
            view_attr = view_type->shape;
        }
        if (elem_type->length > 0) {
            lam::ShapeRef attr = lam::shape_borrow(elem_type->shape);
            while (attr) {
                Item attr_item = map_shape_field_to_item(elem->data, attr.get());
                Item copied_item = deep_copy_internal(attr_item);
                if (view_type) {
                    if (!copy_dom_field(view_attr, attr.get())) return ItemNull;
                    set_field_value(view_attr, map_field_ptr(elem_builder.final().element->data, view_attr), copied_item);
                    view_attr = typemap_next_field(view_type, view_attr);
                } else if (attr->name) {
                    // Copy attribute name bytes from external NamePool
                    String* attr_name = createName(attr->name->str, attr->name->length);
                    elem_builder.attr(copy_name_namespace(pool_, attr->ns), attr_name->chars, copied_item);
                } else {
                    // attr->name is null for nested map
                    elem_builder.attr((String*)nullptr, copied_item);
                }
                attr = lam::shape_next(elem_type, attr);
            }
        }
        if (view_type) typemap_hash_build(view_type, pool_);

        // copy children
        for (int i = 0; i < reader.childCount(); i++) {
            ItemReader child_reader = reader.childAt(i);
            Item child = child_reader.item();
            Item copied_child = deep_copy_internal(child);
            elem_builder.child(copied_child);
        }
        log_leave();
        return elem_builder.final();
    } else if constexpr (Tag == LMD_TYPE_TYPE) {
        if (copying_view_) {
            Type* copied = copy_dom_contract((Type*)typed.ptr());
            return copied ? Item{.type = copied} : ItemError;
        }
        TypeType* source_type = (TypeType*)typed.ptr();
        if (!source_type || !source_type->type) return ItemError;
        if (source_type->type == &TYPE_NUMBER) {
            // `number` is an abstract type singleton; copying by TypeId would collapse it to `type`.
            return typed.raw();
        }
        return createMetaType(source_type->type->type_id);
    } else if constexpr (Tag == LMD_TYPE_PATH) {
        // For sys:// paths, resolve and deep-copy the result
        Path* path = typed.ptr();
        if (path && path_get_scheme(path) == PATH_SCHEME_SYS) {
            // If already resolved, deep-copy the result
            if (path->result != 0) {
                return deep_copy_internal(mark_item_from_bits(path->result));
            }
            // Note: path_resolve_for_iteration is only available in full lambda runtime,
            // not in lambda-input library. Caller is responsible for resolving paths
            // before deep_copy if needed.
        }
        Path* copied = path_clone(pool_, path);
        return copied ? mark_item_from_bits((uint64_t)(uintptr_t)copied) : ItemError;
    } else if constexpr (Tag == LMD_TYPE_ANY || Tag == LMD_TYPE_ERROR || Tag == LMD_TYPE_UNDEFINED) {
        return item;
    } else {
        // opaque callables/host handles retain their separately rooted identity.
        if (copying_view_) return item;
        log_debug("deep_copy_typed: unsupported type_id=%d, returning null", Tag);
        return ItemNull;
    }
}

Item MarkBuilder::deep_copy_unknown(Item item) {
    if (copying_view_) return item;
    TypeId type_id = get_type_id(item);
    log_debug("deep_copy_unknown: unsupported type_id=%d, returning null", type_id);
    return ItemNull;
}

Item MarkBuilder::deep_copy_internal(Item item) {
    // an external fragment can contain owned template results whose identity carries handlers.
    if (preserving_owned_content_ && is_in_arena(item)) return item;
    DeepCopyVisitor visitor = {this};
    return lam::visit(item, visitor);
}

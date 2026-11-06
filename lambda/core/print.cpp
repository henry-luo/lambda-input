#include "data.hpp"
#include "print.h"
#include "lambda-decimal.hpp"
#include "lambda_typed.hpp"
#include "mark_name.hpp"
#include "../io/target.h"
#include "../../lib/log.h"
#include "../../lib/escape.h"
#include "../../lib/str.h"
#include <math.h>
#include <inttypes.h>  // for PRId64
#include <string.h>
#define MAX_DEPTH 2000

static thread_local bool print_diagnostic_only = false;
static thread_local bool print_mark_literals = false;

static char* binary_literal_text(Binary* bin, size_t* text_len) {
    size_t byte_len = binary_length(bin);
    const uint8_t* bytes = binary_data(bin);
    if (byte_len > (SIZE_MAX - 6) / 2) return NULL;
    size_t len = byte_len * 2 + 5;
    char* text = (char*)mem_alloc(len + 1, MEM_CAT_TEMP);
    if (!text) return NULL;
    text[0] = 'b'; text[1] = '\''; text[2] = '\\'; text[3] = 'x';
    // Lambda_Data.md: printed and string() binaries use the canonical
    // uppercase form b'\x<HEX>'
    str_hex_encode_upper(text + 4, (const char*)bytes, byte_len);
    text[len - 1] = '\'';
    text[len] = '\0';
    if (text_len) *text_len = len;
    return text;
}

void format_binary_literal(StrBuf* strbuf, Binary* bin) {
    if (!strbuf) return;
    size_t len = 0;
    char* text = binary_literal_text(bin, &len);
    if (!text) return;
    strbuf_append_str_n(strbuf, text, len);
    mem_free(text);
}

void format_binary_literal_stringbuf(StringBuf* sb, Binary* bin) {
    if (!sb) return;
    size_t len = 0;
    char* text = binary_literal_text(bin, &len);
    if (!text) return;
    stringbuf_append_str_n(sb, text, len);
    mem_free(text);
}

void print_typeditem(StrBuf *strbuf, TypedItem* citem, int depth, const char* indent);
void print_item(StrBuf *strbuf, Item item, int depth, const char* indent);

void print_named_items(StrBuf *strbuf, TypeMap *map_type, void* map_data, int depth = 0, const char* indent = NULL, bool is_attrs = false);

static void print_quoted_text(StrBuf* strbuf, const char* chars, size_t length,
                              char quote, bool escape) {
    if (print_mark_literals && quote == '"') {
        // Mark strings accept JSON escapes, including embedded NUL and other controls.
        escape_append_json_string(strbuf, chars, length, true, false);
        return;
    }
    // escape collection members without double-escaping standalone display text.
    strbuf_append_char(strbuf, quote);
    if (!escape) {
        strbuf_append_str_n(strbuf, chars, length);
        strbuf_append_char(strbuf, quote);
        return;
    }
    escape_append_c_quoted(strbuf, chars, length, quote);
    strbuf_append_char(strbuf, quote);
}

// namespace separators and other non-name bytes need quoting to survive Mark readback.
static void print_mark_name(StrBuf* strbuf, const char* chars, size_t length, char quote = '"') {
    if (mark_is_unquoted_name(chars, length)) strbuf_append_str_n(strbuf, chars, length);
    else print_quoted_text(strbuf, chars, length, quote, true);
}

static void print_range_char(StrBuf* strbuf, int64_t codepoint) {
    char utf8[4];
    size_t length = str_utf8_encode((uint32_t)codepoint, utf8, sizeof(utf8));
    if (length == 0) {
        print_quoted_text(strbuf, "", 0, '"', true);
        return;
    }
    // Boundary libraries share the printer but not the evaluator; render the
    // codepoint directly so char ranges do not introduce an fn_chr link.
    print_quoted_text(strbuf, utf8, length, '"', true);
}

void print_double(StrBuf *strbuf, double num, bool scalar_literal = true) {
    // the shared display dtoa normalizes zero; data literals retain its sign bit.
    if (print_mark_literals && num == 0.0 && signbit(num)) {
        strbuf_append_str(strbuf, "-0.0");
        return;
    }
    char num_buf[64];
    lambda_double_to_shortest(num, num_buf, sizeof(num_buf));
    strbuf_append_str(strbuf, num_buf);
    // a whole-valued float must not read back as an int (including negative zero).
    if (print_mark_literals && scalar_literal && isfinite(num) && !strchr(num_buf, '.') &&
            !strchr(num_buf, 'e') && !strchr(num_buf, 'E')) strbuf_append_str(strbuf, ".0");
}

// The one place `int` values are rendered. C16: poison prints in its own
// parseable spellings (distinct from float's bare inf/nan, so int and float
// output stay separable), and finite ints print as integers at every magnitude
// with no exponent form (spec §4.6).
size_t print_int_value_chars(char* buf, double value) {
    if (LAMBDA_INT_VALUE_IS_POISON(value)) {
        // One spelling: `int` and `float` share these values, so there is no
        // `int.`-prefixed form to print (formal semantics 4).
        const char* text = LAMBDA_INT_VALUE_IS_INF(value) ? "inf" :
            LAMBDA_INT_VALUE_IS_NEG_INF(value) ? "-inf" : "nan";
        size_t len = strlen(text);
        memcpy(buf, text, len + 1);
        return len;
    }
    if (value >= (double)INT53_MIN && value <= (double)INT53_MAX) {
        // digits written directly: the format path ran vsnprintf twice per int
        int64_t v = (int64_t)value;
        size_t len = 0;
        uint64_t magnitude = (uint64_t)v;
        if (v < 0) {
            buf[len++] = '-';
            magnitude = 0 - magnitude;
        }
        len += str_uint64_decimal_write(buf + len, magnitude);
        buf[len] = '\0';
        return len;
    }
    return 0;   // finite and outside the band: the caller uses "%.0f"
}

void print_int_value(StrBuf* strbuf, double value) {
    char buf[PRINT_INT_VALUE_CHARS_CAP];
    size_t len = print_int_value_chars(buf, value);
    if (len > 0) {
        strbuf_append_str_n(strbuf, buf, len);
    } else {
        strbuf_append_format(strbuf, "%.0f", value);
    }
}

// C16: the `int` lane is double-backed, so its poison IS the IEEE special and
// needs no lane sentinel; it renders through the one int renderer. ELEM_INT64
// is the integer domain and keeps its own int64 spelling.
void print_packed_int_elem(StrBuf* strbuf, ArrayNumElemType et, const ArrayNum* arr,
                           int64_t index) {
    if (et == ELEM_INT) {
        // v5: ELEM_INT stores LANE values, so a sentinel maps back to its
        // shared IEEE value before the one int renderer sees it.
        print_int_value(strbuf, lambda_int_lane_to_double(arr->items[index]));
        return;
    }
    strbuf_append_format(strbuf, "%lld", (long long)arr->items[index]);
}

static void print_complex(StrBuf* strbuf, Complex* value) {
    if (!value) { strbuf_append_str(strbuf, "error"); return; }
    // the trailing j already identifies the complex scalar; components need no float suffix.
    print_double(strbuf, value->real, false);
    if (signbit(value->imag)) {
        strbuf_append_char(strbuf, '-');
        print_double(strbuf, fabs(value->imag), false);
    } else {
        strbuf_append_char(strbuf, '+');
        print_double(strbuf, value->imag, false);
    }
    strbuf_append_char(strbuf, 'j');
}

void print_decimal(StrBuf *strbuf, Decimal *decimal) {
    if (!decimal_has_payload(decimal)) { strbuf_append_str(strbuf, "error");  return; }
    const char* special = decimal_special_literal(decimal);
    if (special) { strbuf_append_str(strbuf, special); return; }
    // Use centralized decimal_to_string function
    char *decimal_str = decimal_to_string(decimal);
    if (!decimal_str) { strbuf_append_str(strbuf, "error");  return; }
    // log_debug("printed decimal: %s", decimal_str);
    strbuf_append_str(strbuf, decimal_str);
    if (print_mark_literals)
        strbuf_append_char(strbuf, decimal->storage_kind == DECIMAL_BIGINT ? 'n' : 'm');
    decimal_free_string(decimal_str);
}

// prefixes are lexical hints; canonical field/tag names stay local (S8.2.2v6).
static void print_qualified_mark_name(StrBuf* output, Target* ns, StrView name,
        char quote = '"') {
    if (!ns || !ns->name_prefix || !*ns->name_prefix) {
        print_mark_name(output, name.str, name.length, quote); return;
    }
    if (ns->name_separator == '.' && mark_is_unquoted_name(name.str, name.length)) {
        strbuf_append_str(output, ns->name_prefix);
        strbuf_append_char(output, '.');
        strbuf_append_str_n(output, name.str, name.length);
        return;
    }
    StrBuf* spelling = strbuf_new();
    strbuf_append_str(spelling, ns->name_prefix);
    strbuf_append_char(spelling, ns->name_separator ? ns->name_separator : '.');
    strbuf_append_str_n(spelling, name.str, name.length);
    print_mark_name(output, spelling->str, spelling->length, quote);
    strbuf_free(spelling);
}

void print_named_items(StrBuf *strbuf, TypeMap *map_type, void* map_data, int depth, const char* indent, bool is_attrs) {
    // Prevent infinite recursion
    if (depth > MAX_DEPTH) { strbuf_append_str(strbuf, "[MAX_DEPTH_REACHED]");  return; }
    if (!map_type) { strbuf_append_str(strbuf, "[null map_type]");  return; }
    // A corrupt length is refused; a wide map is not. A 10000-field cap printed
    // every larger input map as this marker, dropping its fields (LR11-4).
    if (map_type->length < 0) {
        strbuf_append_str(strbuf, "[invalid map_type length]");
        return;
    }

    ShapeEntry *field = map_type->shape;
    // log_debug("printing named items: %p, type: %d, length: %ld", map_data, map_type->type_id, map_type->length);
    for (int i = 0; i < map_type->length; i++) {
        // Safety check for valid field pointer
        if (!field || (uintptr_t)field < 0x1000) {
        log_error("invalid field pointer: %p", field);
            strbuf_append_str(strbuf, "[invalid field pointer]");
            break;
        }
        if (i) strbuf_append_char(strbuf, ',');
        void* data = map_field_ptr(map_data, field);
        if (!field->name) { // nested map
            log_debug("nested map at field %d: %p", i, data);
            Map *nest_map = *(Map**)data;
            if (!nest_map) {
                log_error("expected a map, got null pointer at field %d", i);
                strbuf_append_str(strbuf, "[null nested map]");
            } else {
                TypeMap *nest_map_type = (TypeMap*)nest_map->type;
                print_named_items(strbuf, nest_map_type, nest_map->data, depth, indent, is_attrs);
            }
        }
        else {
            // Safety check for field name and type
            if (!field->name || (uintptr_t)field->name < 0x1000) {
                log_error("invalid field name: %p", field->name);
                strbuf_append_str(strbuf, "[invalid field name]");
                goto advance_field;
            }
            if (!field->type || (uintptr_t)field->type < 0x1000) {
                log_error("invalid field type: %p", field->type);
                strbuf_append_str(strbuf, "[invalid field type]");
                goto advance_field;
            }
            // Safety check for type_id range
            TypeId field_type_id = field->type->type_id;
            if (field_type_id > 50) {
                log_error("invalid type_id: %d", field_type_id);
                strbuf_append_str(strbuf, "[invalid type_id]");
                goto advance_field;
            }
            // add indentation if needed
            if (indent && !is_attrs) {
                strbuf_append_str(strbuf, "\n");
                for (int j = 0; j < depth; j++) strbuf_append_str(strbuf, indent);
            } else {
                strbuf_append_str(strbuf, " ");
            }
            print_qualified_mark_name(strbuf, field->ns, *field->name);
            strbuf_append_str(strbuf, ": ");
            // Read typed map slots the same way Map::get() does, so print output
            // cannot drift from normal map access for dynamic or pointer fields.
            print_item(strbuf, map_shape_field_to_item(map_data, field), depth, indent);
        }

        advance_field:
        field = typemap_next_field(map_type, field);
    }
}

void print_typeditem(StrBuf *strbuf, TypedItem *titem, int depth, const char* indent) {
    if (depth > MAX_DEPTH) {
        strbuf_append_str(strbuf, "[MAX_DEPTH_REACHED]");
        return;
    }
    if (!titem) {
        strbuf_append_str(strbuf, "null");
        return;
    }

    switch (titem->type_id) {
    case LMD_TYPE_NULL:
        strbuf_append_str(strbuf, "null");
        break;
    case LMD_TYPE_BOOL:
        strbuf_append_str(strbuf, titem->bool_val ? "true" : "false");
        break;
    case LMD_TYPE_INT:
        print_int_value(strbuf, titem->double_val);
        break;
    case LMD_TYPE_INT64:
        strbuf_append_format(strbuf, "%" PRId64, titem->long_val);
        break;
    case LMD_TYPE_FLOAT:
        print_double(strbuf, titem->double_val);
        break;
    case LMD_TYPE_COMPLEX:
        print_complex(strbuf, (Complex*)(uintptr_t)titem->item);
        break;
    case LMD_TYPE_NUM_SIZED: {
        Item it = {.item = titem->item};
        NumSizedType st = it.get_num_type();
        if (st == NUM_FLOAT16 || st == NUM_FLOAT32) {
            print_double(strbuf, it.get_num_sized_as_double());
        } else {
            strbuf_append_format(strbuf, "%" PRId64, it.get_num_sized_as_int64());
        }
        strbuf_append_str(strbuf, get_num_sized_type_name(st));
        break;
    }
    case LMD_TYPE_UINT64:
        strbuf_append_format(strbuf, "%" PRIu64, titem->uint64_val);
        break;
    case LMD_TYPE_DTIME: {
        DateTime* dt = titem->datetime_ptr;
        strbuf_append_str(strbuf, "t'");
        if (dt) datetime_format_lambda(strbuf, dt);
        strbuf_append_char(strbuf, '\'');
        break;
    }
    case LMD_TYPE_DECIMAL:
        print_decimal(strbuf, titem->decimal);
        break;
    case LMD_TYPE_STRING:
        if (titem->string) {
            print_quoted_text(strbuf, titem->string->chars, titem->string->len,
                '"', depth > 0);
        } else {
            print_quoted_text(strbuf, "", 0, '"', depth > 0);
        }
        break;
    case LMD_TYPE_SYMBOL:
        if (titem->symbol) {
            print_quoted_text(strbuf, titem->symbol->chars, titem->symbol->len,
                '\'', depth > 0);
        } else {
            print_quoted_text(strbuf, "", 0, '\'', depth > 0);
        }
        break;
    case LMD_TYPE_BINARY:
        format_binary_literal(strbuf, titem->binary);
        break;
    case LMD_TYPE_PATH:
        path_to_string(titem->path, strbuf);
        break;
    case LMD_TYPE_ARRAY:  case LMD_TYPE_ARRAY_NUM:
    case LMD_TYPE_RANGE:  case LMD_TYPE_MAP:  case LMD_TYPE_ELEMENT:   {
        // For complex types, create a temporary Item and use existing print_item logic
        Item temp_item = {.item = titem->item};
        print_item(strbuf, temp_item, depth + 1, indent);
        break;
    }
    case LMD_TYPE_ERROR:
        strbuf_append_str(strbuf, "error");
        break;
    default:
        strbuf_append_format(strbuf, "unknown_type_%d", titem->type_id);
        break;
    }
}

struct PrintItemVisitor {
    StrBuf* strbuf;
    int depth;
    const char* indent;

    void unknown(TypeId type_id) const {
        strbuf_append_format(strbuf, "[unknown type %s!!]", get_type_name(type_id));
    }

    void operator()(lam::ItemOf<LMD_TYPE_RAW_POINTER> item) const {
        unknown(item.tag());
    }

    void operator()(lam::ItemOf<LMD_TYPE_NULL> item) const {
        (void)item;
        strbuf_append_str(strbuf, "null");
    }

    void operator()(lam::ItemOf<LMD_TYPE_BOOL> item) const {
        strbuf_append_str(strbuf, item.value() ? "true" : "false");
    }

    void operator()(lam::ItemOf<LMD_TYPE_INT> item) const {
        // C16: an int is carried as its own IEEE bits, so the value view is a
        // double at every magnitude. Poison must be classified before any
        // integer conversion — (int64_t)inf is not a number.
        print_int_value(strbuf, lambda_int_item_value(item.raw()));
    }

    void operator()(lam::ItemOf<LMD_TYPE_INT64> item) const {
        int64_t long_val = item.value();
        log_debug("print int64: %" PRId64, long_val);
        strbuf_append_format(strbuf, "%" PRId64, long_val);
    }

    void operator()(lam::ItemOf<LMD_TYPE_FLOAT> item) const {
        // Float Items may be self-tagged inline values, so decode through Item.
        print_double(strbuf, item.value());
    }

    void operator()(lam::ItemOf<LMD_TYPE_NUM_SIZED> item) const {
        Item raw = item.raw();
        NumSizedType st = raw.get_num_type();
        if (st == NUM_FLOAT16 || st == NUM_FLOAT32) {
            print_double(strbuf, raw.get_num_sized_as_double());
        } else {
            strbuf_append_format(strbuf, "%" PRId64, raw.get_num_sized_as_int64());
        }
    }

    void operator()(lam::ItemOf<LMD_TYPE_UINT64> item) const {
        strbuf_append_format(strbuf, "%" PRIu64, *item.ptr());
    }

    void operator()(lam::ItemOf<LMD_TYPE_DECIMAL> item) const {
        print_decimal(strbuf, item.ptr());
    }

    void operator()(lam::ItemOf<LMD_TYPE_STRING> item) const {
        String* string = item.ptr();
        if (string) {
            print_quoted_text(strbuf, string->chars, string->len, '"', depth > 0);
        } else {
            print_quoted_text(strbuf, "", 0, '"', depth > 0);
        }
    }

    void operator()(lam::ItemOf<LMD_TYPE_SYMBOL> item) const {
        Symbol* symbol = item.ptr();
        if (symbol && symbol->len == 0) {
            // '' is no literal (S2.2.2v2); print the constant's name
            strbuf_append_str(strbuf, "symbol.empty");
        } else if (symbol) {
            print_quoted_text(strbuf, symbol->chars, symbol->len, '\'', depth > 0);
        } else {
            print_quoted_text(strbuf, "", 0, '\'', depth > 0);
        }
    }

    void operator()(lam::ItemOf<LMD_TYPE_DTIME> item) const {
        DateTime* dt = item.ptr();
        if (dt) {
            strbuf_append_str(strbuf, "t'");
            datetime_format_lambda(strbuf, dt);
            strbuf_append_char(strbuf, '\'');
        }
        else {
            strbuf_append_str(strbuf, "[null datetime]");
        }
    }

    void operator()(lam::ItemOf<LMD_TYPE_BINARY> item) const {
        Binary* binary = item.ptr();
        format_binary_literal(strbuf, binary);
    }

    void operator()(lam::ItemOf<LMD_TYPE_COMPLEX> item) const {
        print_complex(strbuf, item.ptr());
    }

    void operator()(lam::ItemOf<LMD_TYPE_RANGE> item) const {
        Range* range = item.ptr();
        log_debug("print range: %p, start: %ld, end: %ld", range, range->start, range->end);
        strbuf_append_char(strbuf, '[');
        for (int64_t i = 0; i < range->length; i++) {
            if (i > 0) strbuf_append_str(strbuf, ", ");
            if (range->is_char) {
                print_range_char(strbuf, range->start + i);
            } else {
                strbuf_append_int64(strbuf, range->start + i);
            }
        }
        strbuf_append_char(strbuf, ']');
    }

    void operator()(lam::ItemOf<LMD_TYPE_ARRAY> item) const {
        Array* array = item.ptr();
        // sparse JS storage needs an explicit projection before Lambda serialization.
        if (container_js_elements_kind(array) == JS_ELEMENTS_SPARSE_TAGGED) {
            print_item(strbuf, ItemError, depth, indent);
            return;
        }
        log_debug("print array: %p, length: %ld", array, array->length);
        strbuf_append_char(strbuf, '[');
        for (int i = 0; i < array->length; i++) {
            if (i) strbuf_append_str(strbuf, ", ");
            Item value = array_item_read(array, i);
            print_item(strbuf, value, depth + 1, indent);
        }
        strbuf_append_char(strbuf, ']');
    }

    // Print one element of an ArrayNum at a flat byte offset (into data buffer).
    void print_array_num_elem(StrBuf* sb, ArrayNum* array, ArrayNumElemType et, int64_t flat_idx) const {
        if (et == ELEM_FLOAT64) {
            print_double(sb, array->float_items[flat_idx]);
        } else if (et == ELEM_INT || et == ELEM_INT64) {
            print_packed_int_elem(sb, et, array, flat_idx);
        } else if (et == ELEM_UINT64) {
            // Printing an owner-backed lane must not consume transient number homes.
            strbuf_append_format(sb, "%" PRIu64, ((uint64_t*)array->data)[flat_idx]);
        } else {
            Item val = array_num_read_borrowed_item(array, flat_idx);
            print_item(sb, val, depth + 1, indent);
        }
    }

    // Recursive N-D printer: walks shape[axis..ndim-1] from a base flat index.
    void print_array_num_nd(StrBuf* sb, ArrayNum* array, ArrayNumElemType et,
                            int64_t* shp, int64_t* str, uint8_t ndim,
                            int axis, int64_t base_idx) const {
        strbuf_append_char(sb, '[');
        int64_t dim = shp[axis];
        int64_t stride = str[axis];
        if (axis == ndim - 1) {
            for (int64_t i = 0; i < dim; i++) {
                if (i) strbuf_append_str(sb, ", ");
                print_array_num_elem(sb, array, et, base_idx + i * stride);
            }
        } else {
            for (int64_t i = 0; i < dim; i++) {
                if (i) strbuf_append_str(sb, ", ");
                print_array_num_nd(sb, array, et, shp, str, ndim, axis + 1, base_idx + i * stride);
            }
        }
        strbuf_append_char(sb, ']');
    }

    void operator()(lam::ItemOf<LMD_TYPE_ARRAY_NUM> item) const {
        ArrayNum* array = item.ptr();
        ArrayNumElemType et = array->get_elem_type();

        // N-D path: traverse via shape/strides for nested output. A strided
        // rank-one view (a row of a transposed matrix) walks its stride too.
        if (array->is_ndim && array->extra) {
            ArrayNumShape* shape = (ArrayNumShape*)(uintptr_t)array->extra;
            if (shape && (shape->ndim >= 2 || !array_num_is_dense(array))) {
                print_array_num_nd(strbuf, array, et,
                                   array_num_shape_dims(shape),
                                   array_num_shape_strides(shape),
                                   shape->ndim, 0, 0);
                return;
            }
        }

        // 1-D flat path (also for ndim=1 owned and views)
        strbuf_append_char(strbuf, '[');
        if (et == ELEM_FLOAT64) {
            for (int i = 0; i < array->length; i++) {
                if (i) strbuf_append_str(strbuf, ", ");
                print_double(strbuf, array->float_items[i]);
            }
        } else if (et == ELEM_INT || et == ELEM_INT64) {
            for (int i = 0; i < array->length; i++) {
                if (i) strbuf_append_str(strbuf, ", ");
                print_packed_int_elem(strbuf, et, array, i);
            }
        } else if (et == ELEM_UINT64) {
            // Printing an owner-backed lane must not consume transient number homes.
            for (int i = 0; i < array->length; i++) {
                if (i) strbuf_append_str(strbuf, ", ");
                strbuf_append_format(strbuf, "%" PRIu64, ((uint64_t*)array->data)[i]);
            }
        } else {
            // compact sized types: read and print each element
            for (int i = 0; i < array->length; i++) {
                if (i) strbuf_append_str(strbuf, ", ");
                Item val = array_num_read_borrowed_item(array, i);
                print_item(strbuf, val, depth + 1, indent);
            }
        }
        strbuf_append_str(strbuf, "]");
    }

    void operator()(lam::ItemOf<LMD_TYPE_MAP> item) const {
        Map* map = item.ptr();
        TypeMap* map_type = (TypeMap*)map->type;
        // D3.1.1 phase 2 / OB8: a NOMINAL map prints as `<Type a: 1>` — its
        // type name is its tag, and that spelling is what round-trips as an
        // object literal. Only a structural map wears braces.
        if (TypeNominal* record = type_nominal_record((Type*)map_type)) {
            print_tagged(strbuf, record->type_name, map_type, map->data, nullptr);
            return;
        }
        strbuf_append_char(strbuf, '{');
        print_named_items(strbuf, map_type, map->data, depth + 1, indent);
        // add closing indentation if we have nested structures
        if (indent && map_type->length > 0) {
            strbuf_append_char(strbuf, '\n');
            for (int i = 0; i < depth; i++) strbuf_append_str(strbuf, indent);
        }
        strbuf_append_char(strbuf, '}');
    }

    void operator()(lam::ItemOf<LMD_TYPE_VMAP> item) const {
        VMap* vm = item.ptr();
        int64_t count = vm->vtable->count(vm->data);
        strbuf_append_char(strbuf, '{');
        for (int64_t i = 0; i < count; i++) {
            if (i > 0) strbuf_append_str(strbuf, ", ");
            Item key = vm->vtable->key_at(vm->data, i);
            Item value = vm->vtable->value_at(vm->data, i);
            if (auto key_str = lam::as<LMD_TYPE_STRING>(key)) {
                String* s = key_str.ptr();
                if (s) print_mark_name(strbuf, s->chars, s->len);
            } else {
                print_item(strbuf, key, depth + 1, indent);
            }
            strbuf_append_str(strbuf, ": ");
            print_item(strbuf, value, depth + 1, indent);
        }
        strbuf_append_char(strbuf, '}');
    }

    void operator()(lam::ItemOf<LMD_TYPE_VARRAY> item) const {
        VArray* array = item.ptr();
        Item error = virtual_content_error({.varray = array});
        if (get_type_id(error) == LMD_TYPE_ERROR) { print_item(strbuf, error, depth + 1, indent); return; }
        int64_t count = varray_count(array);
        strbuf_append_char(strbuf, '[');
        for (int64_t i = 0; i < count; i++) {
            if (i) strbuf_append_str(strbuf, ", ");
            print_item(strbuf, varray_get(array, i), depth + 1, indent);
        }
        strbuf_append_char(strbuf, ']');
    }

    void operator()(lam::ItemOf<LMD_TYPE_VELMT> item) const {
        Velmt* element = item.ptr();
        Item error = virtual_content_error({.velmt = element});
        if (get_type_id(error) == LMD_TYPE_ERROR) { print_item(strbuf, error, depth + 1, indent); return; }
        Item tag = ItemNull;
        if (element && element->vtable && element->vtable->element.tag) {
            element->vtable->element.tag(element->data, &tag);
        }
        const char* tag_chars = is_text_type_id(get_type_id(tag))
            ? tag.get_chars() : "element";
        uint32_t tag_length = is_text_type_id(get_type_id(tag))
            ? tag.get_len() : 7;
        strbuf_append_char(strbuf, '<');
        print_mark_name(strbuf, tag_chars, tag_length, '\'');
        int64_t attr_count = velmt_attr_count(element);
        for (int64_t i = 0; i < attr_count; i++) {
            Item key = ItemNull;
            Item value = ItemNull;
            if (element->vtable->element.attrs.key_at(element->data, i, &key) !=
                    VIRTUAL_OP_OK) continue;
            element->vtable->element.attrs.value_at(element->data, i, &value);
            strbuf_append_str(strbuf, i == 0 ? " " : ", ");
            if (is_text_type_id(get_type_id(key))) {
                print_mark_name(strbuf, key.get_chars(), key.get_len());
            } else {
                print_item(strbuf, key, depth + 1, indent);
            }
            strbuf_append_str(strbuf, ": ");
            print_item(strbuf, value, depth + 1, indent);
        }
        int64_t child_count = velmt_child_count(element);
        if (child_count > 0) {
            if (attr_count > 0) strbuf_append_char(strbuf, ',');
            for (int64_t i = 0; i < child_count; i++) {
                strbuf_append_char(strbuf, ' ');
                print_item(strbuf, velmt_child_get(element, i), depth + 1, indent);
            }
        }
        strbuf_append_char(strbuf, '>');
    }

    // S2.1.3: an element and an object print through one body — `<tag attrs,
    // content>` — because an object IS a nominally-typed element and its
    // literal is the element form. The two differ only in where the tag name
    // lives (TypeElmt::name vs TypeObject::type_name) and where the content
    // list is (the element itself vs the object's side list).
    void print_tagged(StrBuf* strbuf, StrView tag, TypeMap* shape, void* data,
            List* content) const {
        strbuf_append_char(strbuf, '<');
        print_qualified_mark_name(strbuf, typemap_has_element_layout(shape)
            ? ((TypeElmt*)shape)->ns : NULL, tag, '\'');
        int64_t attr_count = shape ? shape->length : 0;
        if (attr_count) {
            print_named_items(strbuf, shape, data, depth + 1, indent, true);
        }
        int64_t child_count = content ? content->length : 0;
        if (child_count) {
            // S16.9.3: the attribute/content boundary comma is a biconditional —
            // emit it exactly when there are both attributes and content, in
            // indent mode too (a bare newline is not a boundary; S16.1.1 gives line
            // breaks no meaning). Content children then juxtapose: any separator
            // between them would re-parse as a statement separator, not as siblings.
            strbuf_append_all(strbuf, 2, attr_count ? "," : "", indent ? "\n" : " ");
            for (long i = 0; i < child_count; i++) {
                if (i) strbuf_append_str(strbuf, indent ? "\n" : " ");
                if (indent) { for (int j=0; j<depth+1; j++) strbuf_append_str(strbuf, indent); }
                print_item(strbuf, content->items[i], depth + 1, indent);
            }
        }
        strbuf_append_char(strbuf, '>');
    }

    void operator()(lam::ItemOf<LMD_TYPE_ELEMENT> item) const {
        Element* element = item.ptr();
        TypeElmt* elmt_type = (TypeElmt*)element->type;
        TypeNominal* record = type_nominal_record((Type*)elmt_type);
        print_tagged(strbuf, record ? record->type_name : elmt_type->name,
            (TypeMap*)elmt_type, element->data,
            (List*)element);
    }

    void operator()(lam::ItemOf<LMD_TYPE_FUNC> item) const {
        Function* func = item.ptr();
        strbuf_append_format(strbuf, "[fn %p]", func);
    }

    void operator()(lam::ItemOf<LMD_TYPE_TYPE> item) const {
        // D4.1.1: a pattern is a bare TypePattern under the TYPE tag, not the
        // TypeType wrapper read below (whose kind is SIMPLE); reading its
        // `type` field crashed. It prints as its canonical source.
        Type* value = (Type*)item.ptr();
        if (value->kind == TYPE_KIND_BINARY_SCHEMA) {
            const StrView& name = ((TypeObject*)value)->type_name;
            strbuf_append_str_n(strbuf, name.str, name.length);
            return;
        }
        if (value->kind == TYPE_KIND_PATTERN) {
            String* source = ((TypePattern*)value)->source;
            if (source) strbuf_append_str_n(strbuf, source->chars, source->len);
            else strbuf_append_str(strbuf, "type");
            return;
        }
        // GM5v2: a grammar prints by name, a rule as `grammar.rule`
        if (TypeGrammar* grammar = lambda_type_grammar(value)) {
            strbuf_append_str_n(strbuf, grammar->name->chars, grammar->name->len);
            return;
        }
        if (TypeGrammarRule* rule = lambda_type_grammar_rule(value)) {
            strbuf_append_str_n(strbuf, rule->grammar->name->chars, rule->grammar->name->len);
            strbuf_append_char(strbuf, '.');
            strbuf_append_str_n(strbuf, rule->name->chars, rule->name->len);
            return;
        }
        TypeType* type = (TypeType*)value;
        if (type->type && type->type->kind == TYPE_KIND_BINARY_SCHEMA) {
            const StrView& name = ((TypeObject*)type->type)->type_name;
            strbuf_append_str_n(strbuf, name.str, name.length);
            return;
        }
        if (const char* alias = type_alias_name(type->type)) {
            strbuf_append_str(strbuf, alias);
            return;
        }
        if (type->type->type_id == LMD_TYPE_NUM_SIZED) {
            strbuf_append_str(strbuf, get_num_sized_type_name((NumSizedType)type->type->kind));
            return;
        }
        if (type->type->kind == TYPE_KIND_BINARY) {
            strbuf_append_str(strbuf, "type");
            return;
        }
        const char* type_name = type_info[type->type->type_id].name;
        if (type->type->type_id == LMD_TYPE_NULL) {
            strbuf_append_format(strbuf, "type.%s", type_name);
        } else {
            strbuf_append_str(strbuf, type_name);
        }
    }

    void operator()(lam::ItemOf<LMD_TYPE_PATH> item) const {
        Path* path = item.ptr();
        if (path_get_scheme(path) == PATH_SCHEME_SYS) {
            if (path->result != 0) {
                print_item(strbuf, {.item = path->result}, depth, indent);
                return;
            }
        }
        path_to_string(path, strbuf);
    }

    void operator()(lam::ItemOf<LMD_TYPE_ERROR> item) const {
        (void)item;
        strbuf_append_str(strbuf, "error");
    }

    void operator()(lam::ItemOf<LMD_TYPE_ANY> item) const {
        (void)item;
        strbuf_append_str(strbuf, "any");
    }

    void operator()(lam::ItemOf<LMD_TYPE_UNDEFINED> item) const {
        unknown(item.tag());
    }

    void operator()(Item item) const {
        unknown(get_type_id(item));
    }
};

void print_item(StrBuf *strbuf, Item item, int depth, const char* indent) {
    // limit depth to prevent infinite recursion
    if (depth > MAX_DEPTH) { strbuf_append_str(strbuf, "[MAX_DEPTH_REACHED]");  return; }
    TypeId physical_type = get_type_id(item);
    if (print_diagnostic_only && is_virtual_container_type_id(physical_type)) {
        // S14.3.1v2: diagnostics must not force lazy content or consume its expansion budget.
        strbuf_append_str(strbuf, physical_type == LMD_TYPE_VARRAY ? "[virtual array]" :
            physical_type == LMD_TYPE_VMAP ? "[virtual map]" : "[virtual element]");
        return;
    }
    if (!item.item) {
        log_debug("TRACE: print_item - item is NULL, appending null");
        strbuf_append_str(strbuf, "null");
        return;
    }

    PrintItemVisitor visitor = {strbuf, depth, indent};
    lam::visit(item, visitor);
}

void print_mark_item(StrBuf* strbuf, Item item) {
    struct MarkLiteralScope {
        bool previous = print_mark_literals;
        MarkLiteralScope() { print_mark_literals = true; }
        ~MarkLiteralScope() { print_mark_literals = previous; }
    } scope;
    // depth one applies the existing quoted-literal escaping to a scalar root too.
    print_item(strbuf, item, 1, nullptr);
}

void print_root_item(StrBuf *strbuf, Item item, const char* indent) {
    // The script's top level is content (S2.6.1): a list reaching the root
    // printer is that content and prints one item per line, without brackets.
    // A list anywhere else prints as its array (S2.5.6, via print_item).
    TypeId type_id = get_type_id(item);
    if (type_id == LMD_TYPE_ARRAY && item.array->is_spreadable) {
        Array *array = item.array;
        for (int i = 0; i < array->length; i++) {
            if (i) strbuf_append_char(strbuf, '\n');
            Item value = array_item_read(array, i);
            print_root_item(strbuf, value, indent);
            // remove the trailing '\n' that print_root_item appends
            if (strbuf->length > 0 && strbuf->str[strbuf->length - 1] == '\n') {
                strbuf->str[--strbuf->length] = '\0';
            }
        }
    } else if (type_id == LMD_TYPE_ARRAY_NUM && item.array_num->is_spreadable) {
        ArrayNum *array = item.array_num;
        ArrayNumElemType et = array->get_elem_type();
        // positions map through the stride of a view (array_num_element_offset)
        if (et == ELEM_FLOAT64) {
            for (int i = 0; i < array->length; i++) {
                if (i) strbuf_append_char(strbuf, '\n');
                print_double(strbuf, array->float_items[array_num_element_offset(array, i)]);
            }
        } else if (et == ELEM_INT || et == ELEM_INT64) {
            for (int i = 0; i < array->length; i++) {
                if (i) strbuf_append_char(strbuf, '\n');
                print_packed_int_elem(strbuf, et, array, array_num_element_offset(array, i));
            }
        } else {
            // compact sized types
            for (int i = 0; i < array->length; i++) {
                if (i) strbuf_append_char(strbuf, '\n');
                Item val = array_num_read_borrowed_item(array,
                    array_num_element_offset(array, i));
                print_item(strbuf, val, 0, indent);
            }
        }
    } else {
        print_item(strbuf, item, 0, indent);
    }
    // append last '\n'
    strbuf_append_char(strbuf, '\n');
}

void log_root_item(Item item, const char* indent) {
#ifndef LAMBDA_NO_CONSOLE_DUMP
    StrBuf *output = strbuf_new_cap(256);
    print_root_item(output, item, indent);
    log_debug("%s", output->str);
    strbuf_free(output);
#endif
}

extern "C" void format_item(StrBuf *strbuf, Item item, int depth, const char* indent) {
    print_item(strbuf, item, depth, indent);
}

// Convenience wrapper for testing - prints to stdout
void print_item(Item item, int depth) {
#ifndef LAMBDA_NO_CONSOLE_DUMP
    StrBuf *strbuf = strbuf_new_cap(1024);
    print_item(strbuf, item, depth, nullptr);
    printf("%s", strbuf->str);
    strbuf_free(strbuf);
#endif
}

// print the type of the AST node
const char* format_type(Type *type) {
    if (!type) { return "null*"; }
    if (type == &TYPE_NUMBER) { return "number"; }
    if (type == &TYPE_NONE) { return "none"; }
    TypeId type_id = type->type_id;
    switch (type_id) {
    case LMD_TYPE_NULL:
        return "void*";
    case LMD_TYPE_ANY:
        return "any";
    case LMD_TYPE_ERROR:
        return "ERROR";
    case LMD_TYPE_BOOL:
        return "bool";
    case LMD_TYPE_INT:
        return "int";
    case LMD_TYPE_INT64:
        return "i64";
    case LMD_TYPE_FLOAT:
        return "float";
    case LMD_TYPE_NUM_SIZED:
        return "num_sized";
    case LMD_TYPE_UINT64:
        return "uint64";
    case LMD_TYPE_DECIMAL:
        return "decimal";
    case LMD_TYPE_STRING:
        return "char*";
    case LMD_TYPE_SYMBOL:
        return "char*";
    case LMD_TYPE_DTIME:
        return "DateTime*";
    case LMD_TYPE_BINARY:
        return "uint8_t*";

    case LMD_TYPE_RANGE:
        return "Range*";
    case LMD_TYPE_ARRAY: {
        TypeArray *array_type = (TypeArray*)type;
        if (array_type->nested && array_type->nested->type_id == LMD_TYPE_INT) {
            return "ArrayInt*";
        } else {
            return "Array*";
        }
    }
    case LMD_TYPE_ARRAY_NUM:
        return "ArrayNum*";
    case LMD_TYPE_MAP:
        return "Map*";
        return "Object*";
    case LMD_TYPE_ELEMENT:
        return "Elmt*";
    case LMD_TYPE_FUNC:
        return "Func*";
    case LMD_TYPE_TYPE:
        return "Type*";
    default:
        return "UNKNOWN";
    }
}

void log_item(Item item, const char* msg) {
#ifndef LAMBDA_NO_CONSOLE_DUMP
    // `--no-log` disables diagnostics entirely; traversing runtime values here
    // can touch representation-specific storage even though the message is discarded.
    // Release builds strip log_debug, so the level check alone printed every closed
    // element's whole subtree for nothing -- O(depth x size) on nested documents.
    if (!log_debug_enabled(log_default_category)) return;
    StrBuf *strbuf = strbuf_new();
    bool previous = print_diagnostic_only;
    print_diagnostic_only = true;
    print_item(strbuf, item, 0, NULL);
    print_diagnostic_only = previous;
    log_debug("%s: %s", msg, strbuf->str);
    strbuf_free(strbuf);
#endif
}

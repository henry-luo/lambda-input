#ifndef LAMBDA_API_H
#define LAMBDA_API_H
// Module API annotations are source-contract markers today.  Static archives
// do not need symbol exporting, but keeping the annotation at each public
// declaration lets the validation DSOs adopt hidden visibility without
// changing their headers again.
#if defined(_WIN32)
#if defined(LAMBDA_BUILD_SHARED)
#define LAMBDA_API __declspec(dllexport)
#elif defined(LAMBDA_USE_SHARED)
#define LAMBDA_API __declspec(dllimport)
#else
#define LAMBDA_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define LAMBDA_API __attribute__((visibility("default")))
#else
#define LAMBDA_API
#endif

#define LAMBDA_LIB_API LAMBDA_API
#define LAMBDA_CORE_API LAMBDA_API
#define LAMBDA_IO_API LAMBDA_API
#define LAMBDA_RT_API LAMBDA_API
#define RADIANT_API LAMBDA_API
#endif // LAMBDA_API_H

// Library headers can request annotations without importing the value ABI.
#if !defined(LAMBDA_API_ONLY) && !defined(LAMBDA_MAIN_HEADER_INCLUDED)
#define LAMBDA_MAIN_HEADER_INCLUDED

// Shared C and C++ value declarations. Direct MIR emission uses the value ABI
// and runtime import signatures; generated modules do not parse this header.

// Include standard integer types from system
#include <stdint.h>
#include <stddef.h>

// Keep a fallback for freestanding C consumers that do not provide stddef.h.
#if !defined(__cplusplus) && !defined(_SIZE_T) && !defined(_SIZE_T_) && !defined(__SIZE_T__) && !defined(_SYS__TYPES_H_)
typedef uint64_t size_t;
#endif

#if !defined(__cplusplus) && !defined(_STDBOOL_H) && !defined(_STDBOOL_H_) && !defined(__bool_true_false_are_defined)
#define bool uint8_t
#define true 1
#define false 0
#endif

#define null 0

// Name pool configuration
#define NAME_POOL_SYMBOL_LIMIT 32  // Max length for symbols in name_pool

enum EnumTypeId {
    LMD_TYPE_RAW_POINTER = 0,
    LMD_TYPE_NULL,
    // JavaScript-specific scalar value, distinct from Lambda null.
    LMD_TYPE_UNDEFINED,  // JavaScript undefined (distinct from null)

    // scalar types
    LMD_TYPE_BOOL,
    // Sized numeric types
    LMD_TYPE_NUM_SIZED,  // inline sized numerics (i8..u32, f16, f32) — packed in Item
    LMD_TYPE_INT,    // int literal, just 32-bit
    LMD_TYPE_INT64,  // int literal, 64-bit
    LMD_TYPE_UINT64, // unsigned 64-bit integer (number-home or owned pointer)
    LMD_TYPE_FLOAT,  // float literal, 64-bit
    LMD_TYPE_DECIMAL,
    // A GC-managed pair of binary64 components.  It is a direct pointer Item
    // so the payload stays distinct from the self-tagged float encoding.
    LMD_TYPE_COMPLEX,
    LMD_TYPE_DTIME,
    LMD_TYPE_SYMBOL,
    LMD_TYPE_STRING,
    LMD_TYPE_BINARY,

    // Path type for file/URL paths
    LMD_TYPE_PATH,  // segmented path with scheme (file, http, https, sys, etc.)

    // container types, LMD_TYPE_CONTAINER
    LMD_TYPE_RANGE,
    LMD_TYPE_ARRAY_NUM,   // unified numeric array (elem_type selects int/int64/float)
    LMD_TYPE_ARRAY,  // array of Items
    LMD_TYPE_VARRAY,  // virtual indexed sequence; public semantic type is array
    LMD_TYPE_MAP,
    LMD_TYPE_VMAP,  // virtual map with vtable dispatch (hashmap, treemap, etc.)
    // D3.1.1 phase 2 (OB14): `LMD_TYPE_OBJECT` is GONE. Nominal-ness is a
    // property of the type descriptor (TypeMap::nominal, cached by
    // Type::is_nominal), never a container kind — a nominal value is an ordinary
    // MAP or ELEMENT, and `object` survives only as a TYPE (`TYPE_OBJECT`),
    // matched by pointer identity.
    LMD_TYPE_ELEMENT,
    LMD_TYPE_VELMT,   // virtual element; public semantic type is element
    LMD_TYPE_TYPE,
    LMD_TYPE_FUNC,

    LMD_TYPE_ANY,
    LMD_TYPE_ERROR,

    LMD_TYPE_COUNT,  // number of semantic type IDs; reserved tags below are excluded
    LMD_CONTAINER_HEAP_START, // special value for container heap entry start

    // D2.1.3/D2.1.5: register occupied Item tags without admitting language types.
    LMD_TYPE_PENDING = 0x1E,   // unresolved scalar return; payload kind selects its lane
    LMD_TYPE_SENTINEL = 0x1F,  // internal JS hole/iterator markers; payload selects the marker
};
typedef uint8_t TypeId;

// Name identity is used by the core property path and by the temporary JS
// Symbol compatibility arm below. The full NamePool API remains in
// core/name_identity.h.
#ifndef LAMBDA_NAME_ID_DEFINED
#define LAMBDA_NAME_ID_DEFINED
typedef uint32_t NameId;
#endif

// Pointer-backed semantic values keep their raw pointer carrier when nullable;
// zero is the lane spelling of null. Numeric/wide scalar tags are deliberately
// excluded because their pointer payloads have distinct ownership rules.
static inline bool lambda_type_id_has_pointer_lane(TypeId type_id) {
    switch (type_id) {
    case LMD_TYPE_DECIMAL:
    case LMD_TYPE_DTIME:
    case LMD_TYPE_SYMBOL:
    case LMD_TYPE_STRING:
    case LMD_TYPE_BINARY:
    case LMD_TYPE_COMPLEX:
    case LMD_TYPE_PATH:
    case LMD_TYPE_RANGE:
    case LMD_TYPE_ARRAY_NUM:
    case LMD_TYPE_ARRAY:
    case LMD_TYPE_VARRAY:
    case LMD_TYPE_MAP:
    case LMD_TYPE_VMAP:
    case LMD_TYPE_ELEMENT:
    case LMD_TYPE_VELMT:
    case LMD_TYPE_TYPE:
    case LMD_TYPE_FUNC:
        return true;
    default:
        return false;
    }
}

// Item tag-space partition for the self-tagged double representation
// (`vibe/Lambda_Type_Double_Boxing.md` §2.3) and the rotated inline-int
// representation (`vibe/Lambda_Type_Int_Boxing.md` §3). The 64-bit space splits
// by its top three bits:
//
//   001-011, 101-111  inline floats — raw IEEE bits, the Item *is* the double
//   000 (0x00-0x1F)   every TypeId tag, raw container pointers, sentinels
//   100 (0x80-0x9F)   inline ints — a double's bits rotated left by one
//
// Non-double Item tags must keep high-byte bits 6 and 5 clear; raw container
// pointers remain bit-identical pointers and are never tagged or masked.
#define ITEM_DBL_MASK        UINT64_C(0x6000000000000000)
#define ITEM_HIGH_BYTE_MASK  UINT64_C(0xFF00000000000000)
#define ITEM_TAG_IS_NON_DOUBLE(tag)  ((((uint8_t)(tag)) & 0x60u) == 0)

// The inline-int octant. A tagged Item is an inline int iff it is not a double
// (bits 62-61 clear) and bit 63 is set, so the discriminator is a sign test on
// the word — see `lambda_item_is_inline_int`. Tags therefore must stay in
// 0x00-0x1F; 0x80-0x9F is no longer tag headroom.
// v5 retired the `100` octant reservation: rotation is gone, so high bytes
// 0x80-0x9F are ordinary reserved tag headroom again. The predicate survives as
// the tag-space assert below (no tag may collide with the retired octant, which
// keeps the JS sentinel relocation of P2 honest and the space re-usable).
#define ITEM_TAG_IS_NOT_INLINE_INT(tag)  ((((uint8_t)(tag)) & 0x80u) == 0)

// Every legal tag byte, TypeIds and reserved sentinel tags alike. Per-tag
// tables must be sized by this, not by LMD_TYPE_COUNT.
#define LAMBDA_TAG_SPACE_SIZE  0x20

#ifdef __cplusplus
#define LAMBDA_STATIC_ASSERT(cond, msg) static_assert((cond), msg)
#else
#define LAMBDA_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)
#endif

LAMBDA_STATIC_ASSERT(sizeof(uintptr_t) == sizeof(uint64_t),
                     "Lambda Item requires 64-bit pointers");
// TypeIds must fit the 000 octant (0x00-0x1F). This is now a hard ceiling, not
// a convenience: 0x80-0x9F used to be spare tag headroom but belongs to the
// inline-int encoding, so a TypeId may never be allocated there.
LAMBDA_STATIC_ASSERT(LMD_TYPE_COUNT <= 0x20,
                     "Lambda TypeIds must stay out of double discriminator space");
LAMBDA_STATIC_ASSERT(LMD_CONTAINER_HEAP_START <= 0x20,
                     "TypeId space must stay inside the 000 octant; 0x80-0x9F is inline-int space");
// D9.5.5: the enum order is an enforced classification ABI. Array-like
// carriers form one closed range, Velmt immediately follows Element, and no
// valid TypeId follows Error.
LAMBDA_STATIC_ASSERT(LMD_TYPE_ARRAY_NUM == LMD_TYPE_RANGE + 1 &&
                     LMD_TYPE_ARRAY == LMD_TYPE_ARRAY_NUM + 1 &&
                     LMD_TYPE_VARRAY == LMD_TYPE_ARRAY + 1,
                     "array-family TypeIds must be the contiguous RANGE..VARRAY band");
LAMBDA_STATIC_ASSERT(LMD_TYPE_VELMT == LMD_TYPE_ELEMENT + 1,
                     "Velmt must immediately follow Element");
LAMBDA_STATIC_ASSERT(LMD_TYPE_COUNT == LMD_TYPE_ERROR + 1,
                     "Error must be the last valid TypeId");
LAMBDA_STATIC_ASSERT(LMD_TYPE_RANGE == 16 && LMD_TYPE_VARRAY == 19 &&
                     LMD_TYPE_VELMT == 23 && LMD_TYPE_ERROR == 27,
                     "D9.5.5 TypeId ABI changed without an ABI revision");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_RAW_POINTER), "raw pointer tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_NULL), "null tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_UNDEFINED), "undefined tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_BOOL), "bool tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_NUM_SIZED), "sized numeric tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_INT), "int tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_INT64), "int64 tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_UINT64), "uint64 tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_FLOAT), "float tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_DECIMAL), "decimal tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_DTIME), "datetime tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_SYMBOL), "symbol tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_STRING), "string tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_BINARY), "binary tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_COMPLEX), "complex tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_PATH), "path tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_RANGE), "range tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_ARRAY_NUM), "array-num tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_ARRAY), "array tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_VARRAY), "varray tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_MAP), "map tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_VMAP), "vmap tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_ELEMENT), "element tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_VELMT), "velmt tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_TYPE), "type tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_FUNC), "function tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_ANY), "any tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE(LMD_TYPE_ERROR), "error tag must be non-double");

// ============================================================================
// Sized numeric sub-types (stored in bits [55:48] of NUM_SIZED Items)
// ============================================================================
enum EnumNumSizedType {
    NUM_INT8 = 0,     // signed 8-bit   [-128, 127]
    NUM_INT16,        // signed 16-bit  [-32768, 32767]
    NUM_INT32,        // signed 32-bit  [-2^31, 2^31-1]
    NUM_UINT8,        // unsigned 8-bit  [0, 255]
    NUM_UINT16,       // unsigned 16-bit [0, 65535]
    NUM_UINT32,       // unsigned 32-bit [0, 2^32-1]
    NUM_FLOAT16,      // IEEE 754 half-precision (16-bit)
    NUM_FLOAT32,      // IEEE 754 single-precision (32-bit)
    NUM_SIZED_COUNT
};
typedef uint8_t NumSizedType;

// ============================================================================
// ArrayNum element sub-types (stored in Container.map_kind/elem_type byte)
// Container lifecycle flags and ArrayNum layout flags live in separate bytes.
// ============================================================================
enum EnumArrayNumElemType {
    // Lambda's standard numeric types (8 bytes/element each):
    ELEM_INT   = 0x00,   // 8 bytes  — v5 IntLane i64 (finite values or poison sentinels)
    ELEM_FLOAT64 = 0x10, // 8 bytes  — canonical double lane (was ARRAY_FLOAT)
    ELEM_FLOAT = ELEM_FLOAT64, // source compatibility alias; not a distinct representation

    // Compact sized integer types:
    ELEM_INT8    = 0x20,  // 1 byte   — maps to NUM_INT8
    ELEM_INT16   = 0x30,  // 2 bytes  — maps to NUM_INT16
    ELEM_INT32   = 0x40,  // 4 bytes  — maps to NUM_INT32
    ELEM_INT64   = 0x50,  // 8 bytes  — int64 (was ARRAY_INT64)

    ELEM_UINT8   = 0x60,  // 1 byte   — maps to NUM_UINT8
    ELEM_UINT16  = 0x70,  // 2 bytes  — maps to NUM_UINT16
    ELEM_UINT32  = 0x80,  // 4 bytes  — maps to NUM_UINT32
    ELEM_UINT64  = 0x90,  // 8 bytes  — maps to NUM_UINT64

    // Compact sized float types:
    ELEM_FLOAT16 = 0xA0,  // 2 bytes  — maps to NUM_FLOAT16
    ELEM_FLOAT32 = 0xB0,  // 4 bytes  — maps to NUM_FLOAT32
    ELEM_RESERVED_C0 = 0xC0, // retired duplicate double lane; keep the slot unavailable

    // Boolean type (1 byte/element):
    ELEM_BOOL    = 0xD0,  // 1 byte   — bool values, distinct from UINT8 for any()/all() semantics

    // JS-compatible clamped byte type:
    ELEM_UINT8_CLAMPED = 0xE0,  // 1 byte — Uint8ClampedArray storage semantics

    ELEM_NUM_COUNT = 14
};
typedef uint8_t ArrayNumElemType;

// Bytes per element, indexed by (elem_type >> 4)
static const uint8_t ELEM_TYPE_SIZE[16] = {
    8, // 0x00 ELEM_INT     — int64_t (v5: the int lane is the machine i64)
    8, // 0x10 ELEM_FLOAT64 — double
    1, // 0x20 ELEM_INT8
    2, // 0x30 ELEM_INT16
    4, // 0x40 ELEM_INT32
    8, // 0x50 ELEM_INT64   — int64_t
    1, // 0x60 ELEM_UINT8
    2, // 0x70 ELEM_UINT16
    4, // 0x80 ELEM_UINT32
    8, // 0x90 ELEM_UINT64
    2, // 0xA0 ELEM_FLOAT16
    4, // 0xB0 ELEM_FLOAT32
    0, // 0xC0 reserved retired duplicate double lane
    1, // 0xD0 ELEM_BOOL
    1, // 0xE0 ELEM_UINT8_CLAMPED
    0, // 0xF0 reserved
};

// Convert NumSizedType to ArrayNumElemType
static inline ArrayNumElemType num_sized_to_elem_type(NumSizedType nst) {
    switch (nst) {
        case NUM_INT8:    return ELEM_INT8;
        case NUM_INT16:   return ELEM_INT16;
        case NUM_INT32:   return ELEM_INT32;
        case NUM_UINT8:   return ELEM_UINT8;
        case NUM_UINT16:  return ELEM_UINT16;
        case NUM_UINT32:  return ELEM_UINT32;
        case NUM_FLOAT16: return ELEM_FLOAT16;
        case NUM_FLOAT32: return ELEM_FLOAT32;
        default:          return ELEM_INT;
    }
}

// Check if an elem_type uses compact (sub-8-byte) storage
static inline int elem_type_is_compact(ArrayNumElemType et) {
    return ELEM_TYPE_SIZE[et >> 4] < 8;
}

// Sized numeric packing: value in [31:0], sub-type in [55:48], type_id in [63:56]
// Bits [47:32] are unused padding.
#define NUM_SIZED_PACK(num_type, val32) \
    (((uint64_t)LMD_TYPE_NUM_SIZED << 56) | ((uint64_t)(num_type) << 48) | ((uint32_t)(val32)))

// Unpack sub-type and raw 32-bit value from a NUM_SIZED Item
#define NUM_SIZED_SUBTYPE(item)  ((uint8_t)(((uint64_t)(item) >> 48) & 0xFF))
#define NUM_SIZED_RAW32(item)    ((uint32_t)((uint64_t)(item) & 0xFFFFFFFF))

// Convenience packers for each sub-type
#define i8_to_item(v)   NUM_SIZED_PACK(NUM_INT8,   (uint32_t)(uint8_t)(int8_t)(v))
#define i16_to_item(v)  NUM_SIZED_PACK(NUM_INT16,  (uint32_t)(uint16_t)(int16_t)(v))
#define i32_to_item(v)  NUM_SIZED_PACK(NUM_INT32,  (uint32_t)(int32_t)(v))
#define u8_to_item(v)   NUM_SIZED_PACK(NUM_UINT8,  (uint32_t)(uint8_t)(v))
#define u16_to_item(v)  NUM_SIZED_PACK(NUM_UINT16, (uint32_t)(uint16_t)(v))
#define u32_to_item(v)  NUM_SIZED_PACK(NUM_UINT32, (uint32_t)(v))

// uint64 Items point at a raw 64-bit payload owned by a number home or an
// explicit persistent owner. Transient producers must use box_uint64_value().
#define u64_to_item(uint64_ptr) \
    lambda_uint64_ptr_to_item_bits((const uint64_t*)(uint64_ptr))

// Get human-readable name for a NumSizedType sub-type
#ifdef __cplusplus
extern "C"
#endif
const char* get_num_sized_type_name(NumSizedType num_type);

// TypeKind is defined in the C++ value model, core/value.hpp.

// Get human-readable name for a TypeId (for error messages)
// Implemented in core/lambda-data.cpp.
#ifdef __cplusplus
extern "C"
#endif
const char* get_type_name(TypeId type_id);

// 3-state boolean:
typedef enum {
    BOOL_FALSE = 0,
    BOOL_TRUE = 1,
    BOOL_ERROR = 2
} BoolEnum;
typedef uint8_t Bool;

// Virtual structural callbacks must distinguish a missing entry from a
// present null and keep readonly host state separate from an execution error.
typedef enum VirtualOpStatus {
    VIRTUAL_OP_OK = 0,
    VIRTUAL_OP_MISSING,
    VIRTUAL_OP_READONLY,
    VIRTUAL_OP_ERROR,
} VirtualOpStatus;

#define  LMD_TYPE_CONTAINER LMD_TYPE_RANGE

typedef struct Type {
    TypeId type_id;
    uint8_t kind:4;      // TypeKind: sub-classification (SIMPLE, UNARY, BINARY, PATTERN)
    uint8_t is_literal:1;  // is a literal value
    uint8_t is_const:1;  // is a constant expr
    // D3.1.1 phase 2: set only on a real TypeMap/TypeElmt that carries a
    // nominal record. It lives on the BASE because "is this type nominal?" must
    // be answerable of ANY Type* — the bare singletons (TYPE_MAP, TYPE_ELMT,
    // LIT_TYPE_MAP, …) share the map/element tag but are plain `Type`s, so
    // reaching for a TypeMap field on them would read past their end.
    uint8_t is_nominal:1;
} Type;

// Immutable proof attached to a homogeneous array carrier. It records the
// full source-language contract independently of the binding that received the
// value, so a native lane never has to recover a named record layout from a
// lossy TypeId.
typedef struct ArrayRepCert ArrayRepCert;

// D3.1.1 / D3.4.1: gc_heap.c is C while the runtime carriers live in C++.
// Keep its byte-level view in one canonical C ABI layout and assert the real
// C and C++ structs against it where their full definitions are available.
typedef struct LambdaGcContainerLayout {
    TypeId type_id;
    uint8_t flags;
    uint8_t array_flags;
    uint8_t map_kind;
    uint8_t cow_state;
    uint8_t ctor_reserved_mask_lo;
    uint8_t ctor_reserved_mask_hi;
    uint8_t reserved_state;
} LambdaGcContainerLayout;

typedef struct LambdaGcMapLayout {
    LambdaGcContainerLayout container;
    void* type;
    void* data;
    int data_cap;
} LambdaGcMapLayout;

// ordered keyed storage keeps strong edges in an ordinary traced Array.
typedef struct LambdaGcOrderedMapLayout {
    LambdaGcMapLayout map;
    void* entries;
    struct hashmap* index;
    int64_t size;
    int64_t cursors;
    int64_t last_entry;
} LambdaGcOrderedMapLayout;

typedef struct LambdaGcListLayout {
    LambdaGcMapLayout map;
    void* items;
    int64_t length;
    int64_t extra;
    int64_t capacity;
} LambdaGcListLayout;

typedef struct LambdaGcTypeMapLayout {
    Type base;
    int64_t length;
    int64_t byte_size;
    int type_index;
    uint8_t has_spread;
    uint8_t has_named_shape;
    uint8_t is_trusted_contract;
    void* shape;
    void* last;  // D3.4.3: a tree node's chain may continue past it
} LambdaGcTypeMapLayout;

typedef struct LambdaGcShapeEntryLayout {
    void* name;
    Type* type;
    int64_t byte_offset;
    void* next;
} LambdaGcShapeEntryLayout;

#pragma pack(push, 1)
typedef struct LambdaGcTypedItemLayout {
    TypeId type_id;
    uint64_t value;
} LambdaGcTypedItemLayout;
#pragma pack(pop)

typedef struct LambdaGcArrayNumShapeLayout {
    uint8_t ndim;
    uint8_t layout_flags;
    uint8_t backing_kind;
    uint8_t backing_padding[5];
    int64_t offset;
    void* base;
    void* backing;
} LambdaGcArrayNumShapeLayout;

struct gc_heap;
typedef struct LambdaGcVMapVtableLayout {
    void* get;
    void* set;
    void* count;
    void* keys;
    void* key_at;
    void* value_at;
    void* destroy;
    void (*trace)(void* data, struct gc_heap* gc);
} LambdaGcVMapVtableLayout;

typedef struct LambdaGcVMapLayout {
    LambdaGcContainerLayout container;
    void* data;
    LambdaGcVMapVtableLayout* vtable;
} LambdaGcVMapLayout;

// VArray and Velmt share this prefix. VMap has the same object-field offsets,
// while retaining its established map-specific vtable ABI.
typedef struct LambdaGcVirtualVtableLayout {
    uint32_t abi_version;
    TypeId carrier_type;
    uint8_t reserved[3];
    void (*destroy)(void* data);
    void (*trace)(void* data, struct gc_heap* gc);
    uint64_t (*version)(void* data);
} LambdaGcVirtualVtableLayout;

typedef struct LambdaGcVirtualContainerLayout {
    LambdaGcContainerLayout container;
    void* data;
    void* vtable;
    const void* host_type;
    void* host_data;
    uint64_t expando;
} LambdaGcVirtualContainerLayout;

typedef struct LambdaGcFunctionLayout {
    TypeId type_id;
    uint8_t arity;
    uint8_t closure_field_count;
    uint8_t entry_abi;
    uint32_t flags;
    void* fn_type;
    void* ptr;
    void* closure_env;
} LambdaGcFunctionLayout;

enum LambdaGcLayoutOffset {
    LAMBDA_GC_OFF_CONTAINER_TYPE_ID = offsetof(LambdaGcContainerLayout, type_id),
    LAMBDA_GC_OFF_CONTAINER_FLAGS = offsetof(LambdaGcContainerLayout, flags),
    LAMBDA_GC_OFF_CONTAINER_ARRAY_FLAGS = offsetof(LambdaGcContainerLayout, array_flags),
    LAMBDA_GC_OFF_CONTAINER_MAP_KIND = offsetof(LambdaGcContainerLayout, map_kind),
    LAMBDA_GC_OFF_MAP_TYPE = offsetof(LambdaGcMapLayout, type),
    LAMBDA_GC_OFF_MAP_DATA = offsetof(LambdaGcMapLayout, data),
    LAMBDA_GC_OFF_MAP_DATA_CAP = offsetof(LambdaGcMapLayout, data_cap),
    LAMBDA_GC_OFF_LIST_ITEMS = offsetof(LambdaGcListLayout, items),
    LAMBDA_GC_OFF_LIST_LENGTH = offsetof(LambdaGcListLayout, length),
    LAMBDA_GC_OFF_LIST_EXTRA = offsetof(LambdaGcListLayout, extra),
    LAMBDA_GC_OFF_LIST_CAPACITY = offsetof(LambdaGcListLayout, capacity),
    LAMBDA_GC_OFF_TYPE_MAP_BYTE_SIZE = offsetof(LambdaGcTypeMapLayout, byte_size),
    LAMBDA_GC_OFF_TYPE_MAP_SHAPE = offsetof(LambdaGcTypeMapLayout, shape),
    LAMBDA_GC_OFF_TYPE_MAP_LAST = offsetof(LambdaGcTypeMapLayout, last),
    LAMBDA_GC_OFF_SHAPE_ENTRY_TYPE = offsetof(LambdaGcShapeEntryLayout, type),
    LAMBDA_GC_OFF_SHAPE_ENTRY_BYTE_OFFSET = offsetof(LambdaGcShapeEntryLayout, byte_offset),
    LAMBDA_GC_OFF_SHAPE_ENTRY_NEXT = offsetof(LambdaGcShapeEntryLayout, next),
    LAMBDA_GC_OFF_TYPED_ITEM_TYPE_ID = offsetof(LambdaGcTypedItemLayout, type_id),
    LAMBDA_GC_OFF_TYPED_ITEM_VALUE = offsetof(LambdaGcTypedItemLayout, value),
    LAMBDA_GC_OFF_ARRAY_NUM_SHAPE_BASE = offsetof(LambdaGcArrayNumShapeLayout, base),
    LAMBDA_GC_OFF_ARRAY_NUM_SHAPE_BACKING = offsetof(LambdaGcArrayNumShapeLayout, backing),
    LAMBDA_GC_OFF_VMAP_DATA = offsetof(LambdaGcVMapLayout, data),
    LAMBDA_GC_OFF_VMAP_VTABLE = offsetof(LambdaGcVMapLayout, vtable),
    LAMBDA_GC_OFF_VMAP_VTABLE_TRACE = offsetof(LambdaGcVMapVtableLayout, trace),
    LAMBDA_GC_OFF_VIRTUAL_DATA = offsetof(LambdaGcVirtualContainerLayout, data),
    LAMBDA_GC_OFF_VIRTUAL_VTABLE = offsetof(LambdaGcVirtualContainerLayout, vtable),
    LAMBDA_GC_OFF_VIRTUAL_HOST_TYPE = offsetof(LambdaGcVirtualContainerLayout, host_type),
    LAMBDA_GC_OFF_VIRTUAL_HOST_DATA = offsetof(LambdaGcVirtualContainerLayout, host_data),
    LAMBDA_GC_OFF_VIRTUAL_EXPANDO = offsetof(LambdaGcVirtualContainerLayout, expando),
    LAMBDA_GC_OFF_VIRTUAL_VTABLE_DESTROY = offsetof(LambdaGcVirtualVtableLayout, destroy),
    LAMBDA_GC_OFF_VIRTUAL_VTABLE_TRACE = offsetof(LambdaGcVirtualVtableLayout, trace),
    LAMBDA_GC_OFF_FUNCTION_CLOSURE_FIELD_COUNT = offsetof(LambdaGcFunctionLayout, closure_field_count),
    LAMBDA_GC_OFF_FUNCTION_CLOSURE_ENV = offsetof(LambdaGcFunctionLayout, closure_env),
};

typedef struct Container Container;
typedef struct Range Range;
typedef struct List List;
typedef struct List Array;
typedef struct ArrayNum ArrayNum;
typedef ArrayNum ArrayInt;    // compat alias: `int` arrays, double lane (elem_type == ELEM_INT)
typedef ArrayNum ArrayInt64;  // compat alias: int64 arrays (elem_type == ELEM_INT64)
typedef ArrayNum ArrayFloat;  // compat alias: float arrays (elem_type == ELEM_FLOAT64)
typedef struct Map Map;
typedef struct SparseArrayMap SparseArrayMap;
typedef struct VMap VMap;
typedef struct VArray VArray;
typedef struct Velmt Velmt;
typedef struct VirtualContainer VirtualContainer;
typedef struct Element Element;
// S2.1.3 / D3.1.1: an object IS a nominally-typed element, so it shares
// Element's layout rather than defining a fourth container shape. The two
// differ only in their TypeId and in which type struct the `type` slot holds
// (TypeObject* vs TypeElmt*), both of which extend TypeMap.
typedef struct Element Object;
typedef struct Function Function;
struct TypeMethod;
typedef struct Decimal Decimal;
typedef struct Complex Complex;
typedef struct TypePattern TypePattern;
typedef struct ByteStorage ByteStorage;
typedef struct ByteBufferHandle ByteBufferHandle;

/*
 * C-compatible Item and data-structure definitions support active C runtime
 * components, including GC, MIR runtime/import handling, and Path. They mirror
 * the C++ layouts in core/value.hpp for the shared runtime ABI; they are not a
 * C2MIR input interface.
 */

// only define DateTime if not already defined by lib/datetime.h
#ifndef __cplusplus
typedef uint64_t DateTime;
typedef uint64_t Item;
#else
#include "lib/datetime.h"
typedef struct Item Item;
#endif

// a fat string with prefixed length and flags
#ifndef STRING_STRUCT_DEFINED
typedef struct String {
    uint32_t len;       // byte length of the string
    union {
        uint8_t flags;
        struct {
            uint8_t is_ascii:1;   // enables O(1) indexing when every byte is ASCII
            uint8_t is_buffer:1;  // exclusively owned GC string-builder storage
            uint8_t is_pooled:1;  // NameRecord metadata prefix immediately precedes this String
            uint8_t buffer_borrows:5; // pending appends retain their pre-RHS value
        };
    };
    char chars[];       // UTF-8 string data (null-terminated)
} String;
#define STRING_STRUCT_DEFINED
LAMBDA_STATIC_ASSERT(sizeof(String) == 8, "String header ABI must remain 8 bytes");
LAMBDA_STATIC_ASSERT(offsetof(String, chars) == 5, "String chars ABI must remain byte 5");
#endif

typedef struct Target Target;  // forward declaration for Symbol.ns

typedef uint8_t SymbolKind;
enum SymbolKindValue {
    SYMBOL_LAMBDA_NAME = 0,
    SYMBOL_JS_UNIQUE_UNDESCRIBED,
    SYMBOL_JS_UNIQUE,
    SYMBOL_JS_REGISTERED,
    SYMBOL_JS_WELL_KNOWN,
};

typedef struct Symbol {
    uint32_t len;       // symbol spelling or JS description length
    SymbolKind kind;    // Lambda textual name or JS Symbol semantic variant
    uint8_t reserved[3];
    union {
        Target* ns;      // active for SYMBOL_LAMBDA_NAME
        NameId name_id;  // active for JS kinds during property-path migration
    };
    char chars[];       // symbol spelling or JS description/registry key
} Symbol;
LAMBDA_STATIC_ASSERT(sizeof(Symbol) == 16, "Symbol header ABI must remain 16 bytes");
LAMBDA_STATIC_ASSERT(offsetof(Symbol, chars) == 16, "Symbol chars ABI must remain byte 16");

static inline bool symbol_is_lambda_name(const Symbol* symbol) {
    return symbol && symbol->kind == SYMBOL_LAMBDA_NAME;
}

static inline bool symbol_has_js_identity(const Symbol* symbol) {
    return symbol && symbol->kind != SYMBOL_LAMBDA_NAME;
}

static inline Target* symbol_lambda_namespace(const Symbol* symbol) {
    return symbol_is_lambda_name(symbol) ? symbol->ns : NULL;
}

enum BinaryFlags {
    BINARY_FLAG_NONE = 0,
    BINARY_FLAG_INLINE = 1u << 0,
};

// Binary is an immutable byte span. Runtime values retain storage; compiler
// constants use inline_bytes so their lifetime remains owned by the script pool.
typedef struct Binary {
    uint32_t len;
    uint8_t is_ascii;
    uint8_t flags;
    uint16_t reserved;
    ByteStorage* storage;
    size_t offset;
    uint8_t inline_bytes[];
} Binary;
typedef struct LambdaSymbolKeyList SymbolKeyList;

// MapKind: discriminates exotic Map sub-types so js_property_get can
// skip 9 cascading sentinel-pointer checks for plain JS objects.
enum MapKind {
    MAP_KIND_PLAIN       = 0,  // regular JS/Lambda object (default, zero-init safe)
    MAP_KIND_TYPED_ARRAY = 1,  // Int8Array, Float64Array, etc.
    MAP_KIND_ARRAYBUFFER = 2,  // ArrayBuffer / SharedArrayBuffer
    MAP_KIND_DATAVIEW    = 3,  // DataView
    MAP_KIND_WEB_API_RESOURCE = 4,  // non-node Web API resources: Range, Selection, styles
    MAP_KIND_CSSOM       = 5,  // Stylesheet, CSSRule, RuleStyleDeclaration
    MAP_KIND_ITERATOR    = 6,  // Synthetic iterator (array, string, typed array)
    MAP_KIND_PROCESS_ENV = 7,  // process.env — coerces all values to strings on set
    MAP_KIND_COLLECTION  = 8,  // Map/Set/WeakMap/WeakSet with native internal data
    MAP_KIND_PROXY       = 9,  // ES6 Proxy object
    MAP_KIND_RESERVED_10 = 10, // retired foreign-document map carrier
    MAP_KIND_ARRAY_PROPS = 11, // array reserved-tail companion map: stores literal
                               // legacy markers (__get_N/__set_N/__nw_N/...)
                               // — bypasses Phase 4 accessor-marker intercept.
    MAP_KIND_CSS_NAMESPACE = 12, // CSS namespace object; ordinary shape-backed
                                 // properties plus CSS-specific method dispatch.
    MAP_KIND_DESC       = 13, // regular JS/Lambda object with descriptor metadata
    MAP_KIND_ARRAY_SPARSE = 14, // array companion map plus numeric sparse hash table
    MAP_KIND_ERROR       = 15, // resting-state LambdaError presented as a JS object
    MAP_KIND_REGEXP      = 16, // RegExp carrier with typed trailing native payload
    MAP_KIND_ASYNC_FRAME = 17, // JSCU10: suspended async activation (internal, no property face)
    MAP_KIND_GENERATOR   = 18, // JSCU9: generator carrier with suspended native state
    MAP_KIND_ORDERED     = 19, // shared ordered Item entries plus a native ordinal index
    MAP_KIND_WEAK        = 20, // WeakRef/FinalizationRegistry with native weak slots
};

#define CONTAINER_FLAG_STATIC (1u << 4)
#define CONTAINER_FLAG_IMMORTAL (1u << 5)
// the list kind bit (`is_spreadable`, S2.5.1v2), for code that tests `flags`
// without a Container pointer -- the JIT's slot-store list test (S2.5.6)
#define CONTAINER_FLAG_SPREADABLE (1u << 1)
#define CONTAINER_ARRAY_FLAG_NDIM (1u << 0)
#define CONTAINER_ARRAY_FLAG_NATIVE_LANE (1u << 4)
// raw masks remain part of the container ABI for code that snapshots `flags`
// without a Container pointer (notably the moving GC).
// D3.1.1/D3.1.1: bit 6 was `has_js_props`, retired when the JS companion map
// moved into the array's own attribute face. It is reserved for the phase-2
// `is_nominal` cache bit (S2.1.4) — the flags byte is otherwise full, and the
// container header must stay eight bytes.
#define CONTAINER_FLAG_NOMINAL_RESERVED (1u << 6)
#define CONTAINER_FLAG_CTOR_RESERVED (1u << 7)

// Lambda COW ownership state lives in the Container header padding. Raw
// mutation APIs deliberately never inspect this byte; only *_cow wrappers do.
enum ContainerCowState {
    COW_STATE_SHARED = 1u << 0,
};

static inline bool map_kind_is_array_props(uint8_t map_kind) {
    return map_kind == MAP_KIND_ARRAY_PROPS || map_kind == MAP_KIND_ARRAY_SPARSE;
}

// Array and List struct defintions needed for for-loop
struct Container {
    TypeId type_id;
    union {
        uint8_t flags;
        struct {
            // lifecycle / allocation flags
            uint8_t is_js_arguments:1;   // LambdaJS: this array is an Arguments object (no Lambda meaning)
            uint8_t is_spreadable:1;     // the list kind bit (S2.5.1v2): set ⇔ the value is a list
            uint8_t is_heap:1;           // whether allocated from runtime heap (vs arena for input docs)
            uint8_t is_data_migrated:1;  // data buffer migrated from input pool to runtime pool (for mutated markup containers)
            uint8_t is_static:1;         // read-only const-pool/static data container
            uint8_t is_immortal:1;       // storage outlives every execution frame (input arena/const pool)
            uint8_t nominal_reserved:1;  // reserved for the D3.1.1 `is_nominal` cache bit
            uint8_t has_ctor_reserved:1; // map has constructor slots awaiting initialization
        };
    };
    union {
        uint8_t array_flags; // ArrayNum flags
        struct {
            uint8_t is_ndim:1;           // bit 0: has shape side-table in `extra` (n-d owned array)
            uint8_t is_view:1;           // bit 1: aliases another container's storage (implies is_ndim)
            uint8_t is_pinned:1;         // bit 2: reserved legacy pin marker; nursery data is always relocated
            uint8_t is_mutable_view:1;   // bit 3: a view writable through to its base (procedural in-place updates)
            uint8_t is_native_lane_array:1; // bit 4: List.items holds native lane words
            // NOT free: bits 5-7 are JsElementsKind, addressed through
            // JS_ELEMENTS_STATE_MASK (0xe0). Taking one corrupts the elements
            // state. `reserved_state` below is the byte with room to spare.
            uint8_t array_flags_js_elements_kind:3;
        };
    };    
    uint8_t map_kind;      // MapKind tag (0 = plain, only used for map/object/element)
    // these named state bytes occupy the former padding without changing the
    // public eight-byte header or any derived-container field offset.
    uint8_t cow_state;
    uint8_t ctor_reserved_mask_lo;
    uint8_t ctor_reserved_mask_hi;
    union {
        uint8_t reserved_state;
        struct {
            uint8_t reserved_state_low:2;
            uint8_t is_virtual:1; // output List extension; independent of list kind
            uint8_t reserved_state_high:5;
        };
    };
};

LAMBDA_STATIC_ASSERT(offsetof(Container, type_id) == LAMBDA_GC_OFF_CONTAINER_TYPE_ID,
                     "Container TypeId must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(Container, flags) == LAMBDA_GC_OFF_CONTAINER_FLAGS,
                     "Container flags must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(Container, array_flags) == LAMBDA_GC_OFF_CONTAINER_ARRAY_FLAGS,
                     "Container array flags must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(Container, map_kind) == LAMBDA_GC_OFF_CONTAINER_MAP_KIND,
                     "Container map kind must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(Container, cow_state) == 4,
                     "Container COW state must reuse padding byte zero");
LAMBDA_STATIC_ASSERT(offsetof(Container, ctor_reserved_mask_lo) == 5,
                     "Container constructor mask low-byte ABI offset changed");
LAMBDA_STATIC_ASSERT(offsetof(Container, ctor_reserved_mask_hi) == 6,
                     "Container constructor mask high-byte ABI offset changed");
LAMBDA_STATIC_ASSERT(offsetof(Container, reserved_state) == 7,
                     "Container reserved-state ABI offset changed");

// `reserved_state` carries per-container state with no room in the two flag
// bytes. Bit 0 marks a strict-mode Arguments object: `is_js_arguments` on the
// same array already means "this is an Arguments object", and this says which kind.
// It used to be an own `__strict_arguments__` property on the companion map,
// which was visible to Object.keys and is still visible to
// Object.getOwnPropertyNames — engine bookkeeping must not be a user property.
#define CONTAINER_STATE_STRICT_ARGUMENTS ((uint8_t)(1u << 0))
// A JS dense-array buffer allocated outside the GC data zones carries an
// intrusive lifetime record. This bit makes finalization owner-directed;
// it must never be set on a native-lane array.
#define CONTAINER_STATE_JS_RUNTIME_ITEMS ((uint8_t)(1u << 1))
#define CONTAINER_STATE_VIRTUAL_LIST ((uint8_t)(1u << 2))

static inline bool container_is_virtual_list(const struct Container* c) {
    // Native array lanes use the whole state byte for their lane contract.
    return c && (c->type_id == LMD_TYPE_ELEMENT ||
        (c->type_id == LMD_TYPE_ARRAY && !(c->array_flags & CONTAINER_ARRAY_FLAG_NATIVE_LANE))) &&
        (c->reserved_state & CONTAINER_STATE_VIRTUAL_LIST) != 0;
}

static inline bool container_is_strict_arguments(const Container* c) {
    return c && (c->reserved_state & CONTAINER_STATE_STRICT_ARGUMENTS) != 0;
}

static inline void container_set_strict_arguments(Container* c) {
    if (c) c->reserved_state |= CONTAINER_STATE_STRICT_ARGUMENTS;
}
LAMBDA_STATIC_ASSERT(sizeof(Container) == 8,
                     "Container header must remain eight bytes");

LAMBDA_STATIC_ASSERT(CONTAINER_FLAG_NOMINAL_RESERVED == (1u << 6),
                     "Container JS-properties mask must match its bitfield");
LAMBDA_STATIC_ASSERT(CONTAINER_FLAG_CTOR_RESERVED == (1u << 7),
                     "Container constructor-reserved mask must match its bitfield");

// Tune5 §5.1: the high three ArrayNum flag bits carry the per-instance
// ordinary-JS elements state.  Low layout/view bits remain ArrayNum-owned;
// zero deliberately means unmanaged or excluded from JS element semantics.
#define JS_ELEMENTS_STATE_MASK ((uint8_t)0xe0u)
typedef enum JsElementsKind {
    JS_ELEMENTS_NONE = 0x00,
    JS_ELEMENTS_PACKED_NUMERIC = 0x20,
    JS_ELEMENTS_PACKED_TAGGED = 0x40,
    JS_ELEMENTS_HOLEY_TAGGED = 0x60,
    JS_ELEMENTS_SPARSE_TAGGED = 0x80,
} JsElementsKind;

static inline JsElementsKind container_js_elements_kind(const Container* c) {
    return c ? (JsElementsKind)(c->array_flags & JS_ELEMENTS_STATE_MASK)
             : JS_ELEMENTS_NONE;
}

static inline void container_set_js_elements_kind(Container* c,
                                                   JsElementsKind kind) {
    if (!c) return;
    c->array_flags = (uint8_t)((c->array_flags & (uint8_t)~JS_ELEMENTS_STATE_MASK) |
                               ((uint8_t)kind & JS_ELEMENTS_STATE_MASK));
}

static inline bool container_js_elements_kind_is_tagged(JsElementsKind kind) {
    return kind == JS_ELEMENTS_PACKED_TAGGED ||
        kind == JS_ELEMENTS_HOLEY_TAGGED || kind == JS_ELEMENTS_SPARSE_TAGGED;
}

#if defined(__cplusplus)
static_assert(JS_ELEMENTS_STATE_MASK == (uint8_t)(0x20u | 0x40u | 0x80u),
              "Tune5 elements state must occupy only reserved high bits");
#else
_Static_assert(JS_ELEMENTS_STATE_MASK == (uint8_t)(0x20u | 0x40u | 0x80u),
               "Tune5 elements state must occupy only reserved high bits");
#endif

// A descriptor carries the full semantic contract for a packed carrier. TypeId
// alone cannot distinguish int from int?, even though both have LMD_TYPE_INT.
//
// SCU7 (D3.2.1, D3.4.6): ONE total resolver, `lambda_lane_storage_desc_for`,
// answers for every well-formed contract. Every other storage decision -- the
// TypeId that decodes a packed slot, the slot width, the "native lane" tests
// the JIT and the array boundary make -- is a projection of this record.
typedef enum LaneStorageKind {
    LANE_STORAGE_INVALID = 0,
    LANE_STORAGE_ITEM,        // one boxed Item word (wide optional scalar ABI, compact NumSized, dynamic `null` slots)
    LANE_STORAGE_INT,         // int lane word (INT_LANE_NULL when nullable)
    LANE_STORAGE_BOOL,        // one byte (2 = null when nullable; also `undefined`)
    LANE_STORAGE_SIZED_I64,   // raw 64-bit integer word: nullable sized ints, full-width int64/uint64
    LANE_STORAGE_FLOAT64,     // raw double (NaN-boxed null when nullable)
    LANE_STORAGE_POINTER,     // raw pointer word (containers, strings, datetime, type, ...)
    LANE_STORAGE_TYPED_ITEM,  // self-describing TypedItem: dynamic values and persistent wide optionals
} LaneStorageKind;

typedef struct LaneStorageDesc {
    Type* semantic_contract;  // non-owning: the full contract the descriptor was derived from
    Type* base_contract;      // non-owning: the payload contract under `?`/`| null`
    uint8_t kind;             // LaneStorageKind
    uint8_t nullable;         // the slot admits null
    uint8_t byte_size;        // packed map width; native Array slots remain 8 bytes
    uint8_t value_domain;     // TypeId that decodes the slot (map_field_to_item et al.)
    // The value has a raw register/slot representation the JIT may address
    // directly. Non-native kinds still describe the packed slot exactly; they
    // are decoded through `value_domain`.
    uint8_t native;
    uint8_t reserved[3];
} LaneStorageDesc;

enum ArrayRepCertFlags {
    ARRAY_REP_CERT_EXACT = 1u << 0,
    ARRAY_REP_CERT_REIFIED = 1u << 1,
    ARRAY_REP_CERT_ERROR_FREE = 1u << 2,
    // some axis has a fixed length (`T[n]`): lengths are not a carrier fact,
    // so every reuse re-checks them against the live value
    ARRAY_REP_CERT_COUNTED = 1u << 3,
    // the resolved numeric leaf carrier proves every admitted value (D4.3.3)
    ARRAY_REP_CERT_PRIMITIVE_VALUES = 1u << 4,
};

struct ArrayRepCert {
    Type* array_contract;       // canonical T[] / T[][] contract
    Type* immediate_element;    // T for T[]; T[] for T[][]
    Type* leaf_element;         // scalar/map leaf after all array ranks
    LaneStorageDesc leaf_lane;  // exact native/boxed storage decision
    uint8_t rank;
    uint8_t flags;
    uint8_t array_num_elem;     // resolved ArrayNumElemType; valid iff has_array_num_lane
    uint8_t has_array_num_lane;
};

// A field's packed slot is a nullable native lane (int?/bool?/float?/T?/...).
static inline bool lane_desc_is_nullable_native(const LaneStorageDesc* desc) {
    return desc && desc->native && desc->nullable;
}

// List/Array flags (stored in List.flags / Array.flags field)

#ifndef __cplusplus
    struct Range {
        TypeId type_id;
        uint16_t flags;
        uint8_t padding[5];  // padding to align to 8 bytes
        //---------------------
        int64_t start;  // inclusive start
        int64_t end;    // inclusive end
        int64_t length;
    };

    // D3.1.1: List/Array extends Map — the attribute face comes FIRST in
    // every container, so a cast to any ancestor is valid by construction.
    struct List {
        // 8-byte Container header, byte-for-byte with `struct Container`.
        TypeId type_id;
        uint8_t flags;
        uint8_t array_flags;
        uint8_t map_kind;
        uint8_t cow_state;
        uint8_t ctor_reserved_mask_lo;
        uint8_t ctor_reserved_mask_hi;
        uint8_t reserved_state;
        //--------------------- Map face
        void* type;       // shape/type info (NULL for a plain array)
        void* data;       // packed attribute data
        int data_cap;     // capacity of the attribute buffer
        //--------------------- list face
        Item* items;  // pointer to items
        int64_t length;  // number of items
        int64_t extra;   // count of reserved tail items (wide scalars)
        int64_t capacity;  // allocated capacity
        ArrayRepCert* rep_cert;  // non-GC full-contract representation proof
    };

    struct ArrayNum {
        // 8-byte Container header. `elem_type` IS the map_kind byte (@3).
        TypeId type_id;
        uint8_t flags;
        uint8_t array_flags;
        uint8_t elem_type;     // ArrayNumElemType, in the map_kind byte
        uint8_t cow_state;
        uint8_t ctor_reserved_mask_lo;
        uint8_t ctor_reserved_mask_hi;
        uint8_t reserved_state;
        //--------------------- Map face (D3.1.1)
        void* type;
        void* data_shape;   // named to avoid colliding with the element union below
        int data_cap;
        //--------------------- numeric buffer
        union {
            int64_t* items;        // for ELEM_INT, ELEM_INT64
            double* float_items;   // for ELEM_FLOAT64
            void* data;            // for compact types (ELEM_INT8, ELEM_UINT8, etc.)
        };
        int64_t length;  // number of elements
        int64_t extra;   // for is_ndim/is_view: ArrayNumShape* in this slot; else count of extra elements
        int64_t capacity;  // allocated capacity
        ArrayRepCert* rep_cert;  // non-GC full-contract representation proof
    };

    // ArrayList definition for MIR runtime (item_keys return type)
#ifndef _ARRAYLIST_STRUCT_DEFINED
#define _ARRAYLIST_STRUCT_DEFINED
    typedef void *ArrayListValue;
    struct _ArrayList {
        ArrayListValue *data;
        int length;
        int _alloced;
    };
#endif

    // Map, Object, Element struct definitions for direct field access optimization
    // Layout must match the C++ structs in core/value.hpp exactly.
    struct Map {
        // 8-byte Container header, byte-for-byte with `struct Container`.
        TypeId type_id;
        uint8_t flags;
        uint8_t array_flags;
        uint8_t map_kind;
        uint8_t cow_state;
        uint8_t ctor_reserved_mask_lo;
        uint8_t ctor_reserved_mask_hi;
        uint8_t reserved_state;
        //---------------------
        void* type;       // TypeMap* — shape/type info
        void* data;       // packed data struct of the map fields
        int data_cap;     // capacity of the data buffer
    };

    struct SparseArrayMap {
        struct Map base;              // must remain first; array props slot points to base
        struct hashmap* sparse_indices; // numeric sparse array data entries
        int64_t sparse_version;       // increments on numeric sparse mutations
    };


    // D3.1.1: Element extends Array extends Map. The attribute face is FIRST,
    // at Map's own offsets; the content face follows. Object is Element.
    struct Element {
        // 8-byte Container header, byte-for-byte with `struct Container`.
        TypeId type_id;
        uint8_t flags;
        uint8_t array_flags;
        uint8_t map_kind;
        uint8_t cow_state;
        uint8_t ctor_reserved_mask_lo;
        uint8_t ctor_reserved_mask_hi;
        uint8_t reserved_state;
        //--------------------- Map face
        void* type;        // TypeElmt*/TypeObject* — attr type/shape
        void* data;        // packed data struct of the attrs
        int data_cap;      // capacity of the data buffer
        //--------------------- list face
        Item* items;       // list content items
        int64_t length;    // number of content items
        int64_t extra;     // count of extra items
        int64_t capacity;  // allocated capacity
        ArrayRepCert* rep_cert;  // non-GC full-contract representation proof
    };

// D3.1.1: the C mirror of the container chain (Map -> List/Array -> Element)
// must match the C++ definitions in core/value.hpp byte for byte. gc_heap.c is C and
// compiles against THIS mirror, so the C++ static_asserts cannot reach it — a
// silent divergence here mistraces the heap. These are that guard.
LAMBDA_STATIC_ASSERT(offsetof(struct Map, type) == LAMBDA_GC_OFF_MAP_TYPE &&
                     offsetof(struct Map, data) == LAMBDA_GC_OFF_MAP_DATA &&
                     offsetof(struct Map, data_cap) == LAMBDA_GC_OFF_MAP_DATA_CAP,
                     "C mirror: Map must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(struct List, type) == LAMBDA_GC_OFF_MAP_TYPE &&
                     offsetof(struct List, data) == LAMBDA_GC_OFF_MAP_DATA &&
                     offsetof(struct List, data_cap) == LAMBDA_GC_OFF_MAP_DATA_CAP &&
                     offsetof(struct List, items) == LAMBDA_GC_OFF_LIST_ITEMS &&
                     offsetof(struct List, length) == LAMBDA_GC_OFF_LIST_LENGTH &&
                     offsetof(struct List, extra) == LAMBDA_GC_OFF_LIST_EXTRA &&
                     offsetof(struct List, capacity) == LAMBDA_GC_OFF_LIST_CAPACITY,
                     "C mirror: List must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(struct Element, type) == LAMBDA_GC_OFF_MAP_TYPE &&
                     offsetof(struct Element, data) == LAMBDA_GC_OFF_MAP_DATA &&
                     offsetof(struct Element, data_cap) == LAMBDA_GC_OFF_MAP_DATA_CAP &&
                     offsetof(struct Element, items) == LAMBDA_GC_OFF_LIST_ITEMS &&
                     offsetof(struct Element, length) == LAMBDA_GC_OFF_LIST_LENGTH &&
                     offsetof(struct Element, extra) == LAMBDA_GC_OFF_LIST_EXTRA &&
                     offsetof(struct Element, capacity) == LAMBDA_GC_OFF_LIST_CAPACITY,
                     "C mirror: Element must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(struct ArrayNum, items) == LAMBDA_GC_OFF_LIST_ITEMS &&
                     offsetof(struct ArrayNum, length) == LAMBDA_GC_OFF_LIST_LENGTH &&
                     offsetof(struct ArrayNum, extra) == LAMBDA_GC_OFF_LIST_EXTRA &&
                     offsetof(struct ArrayNum, capacity) == LAMBDA_GC_OFF_LIST_CAPACITY,
                     "C mirror: ArrayNum must match the GC ABI");
LAMBDA_STATIC_ASSERT(sizeof(struct Element) == sizeof(struct List) &&
                     sizeof(struct List) == sizeof(struct ArrayNum),
                     "C mirror: element/array headers must stay identical");

#endif


// ============================================================================
// ArrayNumShape — side table for N-D arrays and views.
// Pointer stored in ArrayNum.extra when Container.is_ndim or is_view is set.
// Layout:
//   [header fields ...]
//   int64_t shape[ndim]      — element count per dimension
//   int64_t strides[ndim]    — stride per dimension, in *elements* (not bytes)
// Allocated via pool/heap, freed in ArrayNum finalizer.
// ============================================================================
typedef enum ArrayNumBackingKind {
    ARRAY_NUM_BACKING_GC_OWNED = 0,
    ARRAY_NUM_BACKING_GC_VIEW = 1,
    ARRAY_NUM_BACKING_EXTERNAL_BORROWED = 2,
    ARRAY_NUM_BACKING_BUFFER_HANDLE = 3,
    ARRAY_NUM_BACKING_BYTE_STORAGE = 4,
} ArrayNumBackingKind;

#define LAMBDA_ARRAY_NUM_MAX_NDIM 32

typedef struct ArrayNumShape {
    uint8_t  ndim;            // number of dimensions (1..LAMBDA_ARRAY_NUM_MAX_NDIM)
    uint8_t  is_c_contig:1;   // contiguous in row-major
    uint8_t  is_f_contig:1;   // contiguous in column-major
    uint8_t  reserved:6;
    uint8_t  backing_kind;    // ArrayNumBackingKind; never infer backing from base
    uint8_t  backing_padding[5];
    int64_t  offset;          // element offset within base->data (0 for owned)
    void*    base;            // Container* — non-NULL for views; NULL for owned N-D arrays
    void*    backing;         // ByteBufferHandle*/ByteStorage* for tagged external modes
    uint64_t resolved_generation; // last ByteBufferHandle generation cached in ArrayNum.data
    int64_t  data[];          // shape[ndim] followed by strides[ndim] — total 2*ndim entries
} ArrayNumShape;

LAMBDA_STATIC_ASSERT(offsetof(ArrayNumShape, base) == LAMBDA_GC_OFF_ARRAY_NUM_SHAPE_BASE,
                     "ArrayNumShape base must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(ArrayNumShape, backing) == LAMBDA_GC_OFF_ARRAY_NUM_SHAPE_BACKING,
                     "ArrayNumShape backing must match the GC ABI");

static inline int64_t* array_num_shape_dims(ArrayNumShape* s) { return s->data; }
static inline int64_t* array_num_shape_strides(ArrayNumShape* s) { return s->data + s->ndim; }

#ifdef __cplusplus
extern "C" {
#endif
void array_set(Array* arr, int64_t index, Item item);
void array_copy_owned_items(Array* destination, int64_t destination_index,
                            const Item* source, int64_t count);
void list_relocate_owned_tail(List* list, Item* old_items, int64_t old_capacity,
                              Item* new_items, int64_t new_capacity);
void owned_item_slot_store(Item* storage, int64_t item_count,
                           int64_t index, Item item);
Item owned_item_slot_read(Item* storage, int64_t item_count,
                          int64_t index, bool immortal);
Item js_owned_item_slot_read(Item* storage, int64_t item_count,
                             int64_t index, bool immortal);
#ifdef __cplusplus
}
#endif

typedef void* (*fn_ptr)();

// Core Lambda's native dynamic dispatcher expands individual boxed ABI
// operands.  This is a language limit, not storage capacity: longer values
// must travel in an array/map or a single rest argument.
enum { LAMBDA_MAX_FUNCTION_ARGS = 16 };

// Keys in one fixed-key path write: a compiled path's segments plus its
// terminal (AST_COW_PATH_MAX + 1). Index-key bits ride in shape bits 16..63.
enum { LAMBDA_PATH_KEYS_MAX = 33 };

// Every Core Function value explicitly describes the entry stored in ptr.
// Keep this unversioned layout local to the current JIT process: persistent
// AOT compatibility is deliberately deferred rather than guessed.
typedef uint8_t FunctionEntryAbi;
enum {
    FN_ENTRY_ABI_UNKNOWN = 0,
    FN_ENTRY_ABI_LAMBDA_DIRECT_ONLY,
    FN_ENTRY_ABI_LAMBDA_BOXED_FUNCTION,
    FN_ENTRY_ABI_LAMBDA_BOXED_PROCEDURE,
    FN_ENTRY_ABI_FOREIGN,
    FN_ENTRY_ABI_HOST_ADAPTER,
    // T0 (AI7): a cold Lambda function with no native entry. `ptr` is NULL and
    // `def` carries the AST definition site that `interp_call` evaluates.
    FN_ENTRY_ABI_LAMBDA_INTERPRETED,
    // Hosted-language callables share this record's 8-byte prefix and are
    // discriminated here rather than by a per-language magic word: every value
    // at or above FN_ENTRY_ABI_HOSTED_FIRST uses that language's layout, not
    // `Function`'s. Consumers read `type_id` at 0 and `entry_abi` at 3.
    FN_ENTRY_ABI_MVP_LMD,  // common Function layout, boxed MVP MIR entry
    FN_ENTRY_ABI_HOSTED_FIRST,
    FN_ENTRY_ABI_JS_FUNCTION = FN_ENTRY_ABI_HOSTED_FIRST,
};

// RVO13: public boxed entries publish their post-call companion contract so
// dynamic dispatch can skip slot resolution for proven shape-1 results.
enum {
    LAMBDA_MIR_PUBLIC_RETURN_UNKNOWN = 0,
    LAMBDA_MIR_PUBLIC_RETURN_ITEM = 1,
    LAMBDA_MIR_PUBLIC_RETURN_ITEM_COMPANION = 2,
};
#define LAMBDA_MIR_PUBLIC_RETURN_SHAPE_SHIFT 7

// Function as first-class value
// Supports both direct function references and closures
struct Function {
    // common callable ABI, inherited by JsFunction. Generated code and GC read
    // type_id at byte 0, closure_field_count at byte 2, and entry_abi at byte 3.
    TypeId type_id;
    uint8_t arity;
    uint8_t closure_field_count;
    FunctionEntryAbi entry_abi;
    union {
        uint32_t flags;
        struct {
            uint32_t reserved_bit_ret_item : 1;
            uint32_t has_kwargs : 1;
            uint32_t is_generator : 1;
            uint32_t is_coroutine : 1;
            uint32_t is_system_function_ref : 1;
            uint32_t requires_scalar_result_home : 1;
            uint32_t requires_runtime_context : 1;
            uint32_t mir_public_return_shape : 2;
            uint32_t reserved_flags : 23;
        };
    };

    // Lambda-specific tail. JsFunction inherits this storage today; future
    // tail unification must prove matching ownership, GC and call semantics
    // before reusing any field rather than treating same-shaped fields alike.
    void* fn_type;        // fn type definition (TypeFunc*)
    fn_ptr ptr;           // native function pointer
    void* closure_env;    // closure environment (NULL if no captures)
    const char* name;     // function name for stack traces (may be NULL)
    struct Context* runtime_context; // owner passed through generated calls
    // Trailing only: generated code pokes type_id at offset 0 and
    // closure_field_count at offset 2, so no field may shift.
    // AST definition site (AstFuncNode*) — the T0 body plus, with `def_module`,
    // the (module, node) identity D7.2.1/S5.5.1 already require. NULL for
    // natively-compiled and foreign entries.
    const void* def;
    struct Script* def_module;  // owner of def's const_list / type_list / slab
    // Non-null only for a T0 bound object method. Its receiver lives in the
    // one-field closure environment so the collector traces it like a capture.
    const struct TypeMethod* method;
};

LAMBDA_STATIC_ASSERT(__builtin_offsetof(Function, type_id) == 0,
                     "Function TypeId must remain at byte zero");
LAMBDA_STATIC_ASSERT(__builtin_offsetof(Function, fn_type) == 8,
                     "Function metadata must preserve pointer alignment");
LAMBDA_STATIC_ASSERT(offsetof(Function, closure_field_count) ==
                         LAMBDA_GC_OFF_FUNCTION_CLOSURE_FIELD_COUNT,
                     "Function closure count must match the GC ABI");
LAMBDA_STATIC_ASSERT(offsetof(Function, closure_env) == LAMBDA_GC_OFF_FUNCTION_CLOSURE_ENV,
                     "Function closure environment must match the GC ABI");

static inline void function_init_abi(Function* function, TypeId type_id,
                                     FunctionEntryAbi entry_abi) {
    if (!function) return;
    function->type_id = type_id;
    function->arity = 0;
    function->closure_field_count = 0;
    function->entry_abi = entry_abi;
    function->flags = 0;
}

static inline bool function_has_abi(const Function* function,
                                    FunctionEntryAbi entry_abi) {
    return function && function->type_id == LMD_TYPE_FUNC &&
        function->entry_abi == entry_abi;
}

// Forward declaration for Pool (full definition at line ~359)
typedef struct Pool Pool;

// Path: segmented symbol for file/URL paths
// A path is a linked chain of segments from leaf to root
// Example: file.etc.hosts -> Path("hosts") -> Path("etc") -> Path("file") -> ROOT
typedef struct Path Path;
typedef struct PathMeta PathMeta;

// Path/PathMeta definitions, enums, macros, and most Path API are in
// core/lambda-path.h. Name and Target are in core/value.hpp and io/target.h.

// Path construction API (called by JIT-generated code)
Path* path_new(Pool* pool, int scheme);                           // Create new path with scheme
Path* path_new_authority(Pool* pool, int scheme, const char* authority);
Path* path_clone(Pool* pool, Path* source);                       // Clone immutable path spine into pool
Path* path_extend(Pool* pool, Path* base, const char* segment);   // Extend path with segment
Path* path_extend_len(Pool* pool, Path* base, const char* segment, size_t len);
Path* path_extend_int(Pool* pool, Path* base, int64_t value);
Path* path_select_parent(Pool* pool, Path* base);
Path* path_select_root(Pool* pool, Path* base);
Path* path_qualify_default(Pool* pool, Path* path);
bool path_file_authority_is_local(Path* path);
bool path_equal(Path* left, Path* right);
uint64_t path_hash(Path* path, uint64_t seed0, uint64_t seed1);
Path* path_concat(Pool* pool, Path* base, Path* suffix);          // Concatenate two paths
Path* path_wildcard(Pool* pool, Path* base);                      // Add * wildcard segment
Path* path_wildcard_recursive(Pool* pool, Path* base);            // Add ** wildcard segment

// Memory allocation for closure environments
typedef struct Context Context;

#ifdef __cplusplus
extern "C" {
#endif
const uint8_t* binary_data(const Binary* binary);
uint32_t binary_length(const Binary* binary);
bool binary_is_ascii(const Binary* binary);
Binary* pool_binary_from_bytes(Pool* pool, const void* src, size_t len);
#ifdef __cplusplus
}
#endif

#define LAMBDA_INT64_MAX    (INT64_MAX - 1)

// DateTime error sentinel — all bits set = clearly invalid
// month=15 (impossible: months are 1-12), every field at maximum
// Used by DateTime-returning functions to signal errors
#define DATETIME_ERROR_VALUE  0xFFFFFFFFFFFFFFFFULL

// Check if a DateTime value is the error sentinel
// Works in both C (uint64_t) and C++ (struct with int64_val)
#ifdef __cplusplus
#define DATETIME_IS_ERROR(dt) ((dt).int64_val == DATETIME_ERROR_VALUE)
#else
#define DATETIME_IS_ERROR(dt) ((dt) == DATETIME_ERROR_VALUE)
#endif

// Create a DateTime error value
#ifdef __cplusplus
#define DATETIME_MAKE_ERROR() (DateTime{.int64_val = DATETIME_ERROR_VALUE})
#else
#define DATETIME_MAKE_ERROR() ((DateTime)DATETIME_ERROR_VALUE)
#endif

#define ITEM_UNDEFINED      0
#define ITEM_NULL           ((uint64_t)LMD_TYPE_NULL << 56)
#define ITEM_NULL_SPREADABLE ((uint64_t)LMD_TYPE_NULL << 56 | 1)  // spreadable null (skip when spreading)
#define ITEM_JS_UNDEFINED   ((uint64_t)LMD_TYPE_UNDEFINED << 56)  // JavaScript undefined
#define ITEM_JS_TDZ         ((uint64_t)LMD_TYPE_UNDEFINED << 56 | 1)  // TDZ sentinel for let/const
// Internal call-ABI marker.  It never reaches a Lambda binding: public MIR
// wrappers replace it with an optional null or evaluate the declared default.
#define ITEM_MISSING_ARGUMENT ((uint64_t)LMD_TYPE_UNDEFINED << 56 | 3)
// reserved Item tag, recorded in EnumTypeId but outside its semantic type range.
// the payload distinguishes internal markers; no language value carries this tag.
#define ITEM_SENTINEL_TAG   ((uint64_t)LMD_TYPE_SENTINEL)
#define ITEM_JS_DELETED_SENTINEL   ((ITEM_SENTINEL_TAG << 56) | UINT64_C(0x00DEAD00DEAD00))
#define ITEM_JS_ITER_DONE_SENTINEL ((ITEM_SENTINEL_TAG << 56) | UINT64_C(0x00DEAD00000000))
// Internal-only AsyncIteratorClose marker. It never crosses the protocol
// boundary: the caller consumes it before awaiting a callable `return()`.
#define ITEM_JS_ITER_CLOSE_ABSENT_SENTINEL ((ITEM_SENTINEL_TAG << 56) | UINT64_C(0x00DEAD00000001))

// ---------------------------------------------------------------------------
// Pending Items — return-value convention v3 (RV3, see
// `vibe/Lambda_Design_Compiling_Return_Value.md`, formal spec D6.2.1 /
// D7.4.2).
//
// A shape-2 return is the pair `[item, scalar]`: lane 1 carries an ordinary
// Item unless the returned value is a WIDE scalar (int64 / uint64 /
// out-of-band double), in which case lane 1 is a PENDING Item and lane 2
// carries the raw 64-bit payload. The caller resolves the pair at the first
// resolution point (consume, patch on escape, or tail-forward).
//
// Two protocol invariants (RV4) that every consumer may rely on:
//   1. A pending Item NEVER lives in memory. It exists only in registers
//      between a call returning and its first resolution point, so anything
//      loaded from a variable, container, field or constant is resolved by
//      construction.
//   2. At most one pending value is live at any point — lane 2 is a single
//      location clobbered by the next call.
//
// 0x1E is the last free byte of the `000` octant (TypeIds run through 0x1B,
// COUNT/HEAP_START take 0x1C-0x1D, ITEM_SENTINEL_TAG takes 0x1F). Spending it
// exhausts the octant; the next tag-byte consumer must take the reserved
// 0x80-0x9F headroom (`Lambda_Type_Double_Boxing.md` Part 8).
//
// the tag is outside the semantic TypeId range. Per-tag tables reserve its
// slot, but it carries no value type and must be resolved before semantic use.
//
// Return-value convention v3 is the only generated-function ABI. Shape-2
// payloads use a MIR pair or Context::mir_companion_slot; side-number-stack
// addresses remain only at explicit native/host ownership boundaries
// (D6.2.1, D6.2.2). The fixed revision rejects stale cached modules.
#define LAMBDA_RETURN_CONVENTION_REVISION 3

#define ITEM_PENDING_TAG    ((uint64_t)LMD_TYPE_PENDING)
#define ITEM_PENDING        (ITEM_PENDING_TAG << 56)
// Payload kind lives in the low bits of lane 1; lane 2 holds the raw bits.
// Kind 3 stays reserved: RV8 rules DTIME in-band (pointer Item, GC-managed),
// and every other pointer-backed scalar is likewise never pending.
#define PENDING_KIND_INT64   UINT64_C(0)
#define PENDING_KIND_UINT64  UINT64_C(1)
#define PENDING_KIND_FLOAT   UINT64_C(2)  // out-of-band double, raw bits on lane 2
#define PENDING_KIND_MASK    UINT64_C(3)
#define ITEM_PENDING_INT64   (ITEM_PENDING | PENDING_KIND_INT64)
#define ITEM_PENDING_UINT64  (ITEM_PENDING | PENDING_KIND_UINT64)
#define ITEM_PENDING_FLOAT   (ITEM_PENDING | PENDING_KIND_FLOAT)

// The discriminator the JIT mirrors in two instructions:
//   and t, item, ITEM_HIGH_BYTE_MASK; beq L_pending, t, ITEM_PENDING
static inline bool lambda_item_is_pending(uint64_t bits) {
    return (bits & ITEM_HIGH_BYTE_MASK) == ITEM_PENDING;
}

static inline uint64_t lambda_item_pending_kind(uint64_t bits) {
    return bits & PENDING_KIND_MASK;
}
// ---------------------------------------------------------------------------

#define ITEM_INT            ((uint64_t)LMD_TYPE_INT << 56)
#define ITEM_INT64          ((uint64_t)LMD_TYPE_INT64 << 56)
// BigInt reuses LMD_TYPE_DECIMAL; Decimal::storage_kind supplies the subtype.
#define ITEM_ERROR          ((uint64_t)LMD_TYPE_ERROR << 56)

// numeric type check: `number` is a type-language union, not a runtime TypeId.
static inline bool is_numeric_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_INT || type_id == LMD_TYPE_INT64 ||
           type_id == LMD_TYPE_UINT64 || type_id == LMD_TYPE_FLOAT ||
           type_id == LMD_TYPE_DECIMAL || type_id == LMD_TYPE_NUM_SIZED ||
           type_id == LMD_TYPE_COMPLEX;
}

// Can a value of this DECLARED type ever be a wide scalar — one that needs a
// number home (v1) or the companion lane (v3)? Everything else is inline,
// pointer-backed or a container, and so needs no rehoming at any boundary.
// This is the single source of the "NONE" decision: the shared
// `jit_scalar_return_class_for_type()` uses it for both emission and the
// C-side sys-func metadata fallback, so the two cannot drift.
static inline bool lambda_type_id_may_be_wide_scalar(TypeId type_id) {
    return type_id == LMD_TYPE_FLOAT ||
           type_id == LMD_TYPE_INT64 || type_id == LMD_TYPE_UINT64 ||
           type_id == LMD_TYPE_ANY;
}

static inline bool is_native_numeric_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_INT || type_id == LMD_TYPE_INT64 ||
           type_id == LMD_TYPE_FLOAT;
}

static inline bool is_integer_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_INT || type_id == LMD_TYPE_INT64;
}

static inline bool is_float_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_FLOAT;
}

static inline bool is_native_numeric_or_bool_type_id(TypeId type_id) {
    return is_native_numeric_type_id(type_id) || type_id == LMD_TYPE_BOOL;
}

static inline bool is_text_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_STRING || type_id == LMD_TYPE_SYMBOL;
}

// S2.5.8: text -- a string or symbol by code point, a binary by byte -- is
// walked as a sequence by every sequence operation and placed as one value by
// `++` and the appends.
static inline bool is_sequence_text_type_id(TypeId type_id) {
    return is_text_type_id(type_id) || type_id == LMD_TYPE_BINARY;
}

static inline TypeId item_semantic_type_id(TypeId type_id) {
    if (type_id == LMD_TYPE_VMAP) return LMD_TYPE_MAP;
    if (type_id == LMD_TYPE_VARRAY) return LMD_TYPE_ARRAY;
    if (type_id == LMD_TYPE_VELMT) return LMD_TYPE_ELEMENT;
    return type_id;
}

static inline bool is_materialized_array_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_ARRAY || type_id == LMD_TYPE_ARRAY_NUM;
}

static inline bool is_array_family_type_id(TypeId type_id) {
    return type_id >= LMD_TYPE_RANGE && type_id <= LMD_TYPE_VARRAY;
}

static inline bool is_map_family_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_MAP || type_id == LMD_TYPE_VMAP ||
           type_id == LMD_TYPE_ELEMENT || type_id == LMD_TYPE_VELMT;
}

// The kind that carries BOTH an attribute face and a content face. Since
// D3.1.1 gives the materialized face to ELEMENT; D9.5.5 adds VELMT as its
// representation-transparent virtual counterpart.
static inline bool is_element_family_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_ELEMENT || type_id == LMD_TYPE_VELMT;
}

static inline bool is_virtual_container_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_VMAP || type_id == LMD_TYPE_VARRAY ||
           type_id == LMD_TYPE_VELMT;
}

static inline bool is_container_type_id(TypeId type_id) {
    return type_id >= LMD_TYPE_CONTAINER && type_id <= LMD_TYPE_VELMT;
}

static inline bool is_native_param_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_INT || type_id == LMD_TYPE_FLOAT ||
           type_id == LMD_TYPE_BOOL || type_id == LMD_TYPE_STRING ||
           type_id == LMD_TYPE_INT64 || type_id == LMD_TYPE_UINT64;
}

static inline bool is_typed_wrapper_param_type_id(TypeId type_id) {
    return type_id == LMD_TYPE_INT || type_id == LMD_TYPE_INT64 ||
           type_id == LMD_TYPE_UINT64 ||
           type_id == LMD_TYPE_FLOAT || type_id == LMD_TYPE_BOOL ||
           type_id == LMD_TYPE_STRING || type_id == LMD_TYPE_BINARY ||
           type_id == LMD_TYPE_SYMBOL || type_id == LMD_TYPE_DECIMAL ||
           type_id == LMD_TYPE_DTIME || type_id == LMD_TYPE_MAP ||
           type_id == LMD_TYPE_ELEMENT;
}

static inline bool is_fn_call_wrapper_return_type_id(TypeId type_id) {
    return is_integer_type_id(type_id) ||
           type_id == LMD_TYPE_UINT64 ||
           type_id == LMD_TYPE_FLOAT || type_id == LMD_TYPE_BOOL ||
           type_id == LMD_TYPE_STRING || type_id == LMD_TYPE_BINARY ||
           type_id == LMD_TYPE_SYMBOL || type_id == LMD_TYPE_DECIMAL ||
           type_id == LMD_TYPE_DTIME;
}

#define IS_NUMERIC_ID(t) is_numeric_type_id((TypeId)(t))

#define ITEM_TRUE           (((uint64_t)LMD_TYPE_BOOL << 56) | (uint8_t)1)
#define ITEM_FALSE          (((uint64_t)LMD_TYPE_BOOL << 56) | (uint8_t)0)
#define ITEM_FLOAT_P0       ((uint64_t)LMD_TYPE_FLOAT << 56)
#define ITEM_FLOAT_N0       (ITEM_FLOAT_P0 | UINT64_C(1))

#ifndef __cplusplus
LAMBDA_STATIC_ASSERT(sizeof(Item) == sizeof(uint64_t), "C Item must remain one word");

#endif
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_NULL >> 56)), "ITEM_NULL tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_NULL_SPREADABLE >> 56)), "ITEM_NULL_SPREADABLE tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_JS_UNDEFINED >> 56)), "ITEM_JS_UNDEFINED tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_JS_TDZ >> 56)), "ITEM_JS_TDZ tag must be non-double");
// (the Jube lazy marker is an ordinary int value, so its encoding is whatever
//  the int encoder produces — nothing tag-specific left to assert here)
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_JS_DELETED_SENTINEL >> 56)), "ITEM_JS_DELETED_SENTINEL tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_JS_ITER_DONE_SENTINEL >> 56)), "ITEM_JS_ITER_DONE_SENTINEL tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_JS_ITER_CLOSE_ABSENT_SENTINEL >> 56)), "ITEM_JS_ITER_CLOSE_ABSENT_SENTINEL tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_INT >> 56)), "ITEM_INT tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_ERROR >> 56)), "ITEM_ERROR tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_TRUE >> 56)), "ITEM_TRUE tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)(ITEM_FALSE >> 56)), "ITEM_FALSE tag must be non-double");
LAMBDA_STATIC_ASSERT((ITEM_FLOAT_P0 & ITEM_DBL_MASK) == 0, "packed +0 must be outside double space");
LAMBDA_STATIC_ASSERT((ITEM_FLOAT_N0 & ITEM_DBL_MASK) == 0, "packed -0 must be outside double space");
LAMBDA_STATIC_ASSERT((uint8_t)(ITEM_FLOAT_P0 >> 56) == LMD_TYPE_FLOAT, "packed +0 must carry float tag");
LAMBDA_STATIC_ASSERT((uint8_t)(ITEM_FLOAT_N0 >> 56) == LMD_TYPE_FLOAT, "packed -0 must carry float tag");
// reserved enum members stay outside semantic types and retain their Item ABI.
LAMBDA_STATIC_ASSERT(LMD_TYPE_PENDING == 0x1E && LMD_TYPE_SENTINEL == 0x1F,
                     "reserved Item tag bytes must retain their ABI values");
LAMBDA_STATIC_ASSERT(ITEM_SENTINEL_TAG > LMD_CONTAINER_HEAP_START &&
                     ITEM_SENTINEL_TAG < LAMBDA_TAG_SPACE_SIZE,
                     "sentinel tag must be reserved within the Item tag space");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NOT_INLINE_INT((uint8_t)ITEM_SENTINEL_TAG),
                     "sentinel tag must stay out of the inline-int octant");
LAMBDA_STATIC_ASSERT(ITEM_JS_DELETED_SENTINEL != ITEM_JS_ITER_DONE_SENTINEL &&
                     ITEM_JS_DELETED_SENTINEL != ITEM_JS_ITER_CLOSE_ABSENT_SENTINEL &&
                     ITEM_JS_ITER_DONE_SENTINEL != ITEM_JS_ITER_CLOSE_ABSENT_SENTINEL,
                     "internal sentinels must stay distinct");
// The pending tag (RV3) has the same unreachability requirements as the
// sentinel tag, plus it must stay distinct from the sentinel family: JS
// deleted/iter-done sentinels are legitimately STORED (sparse arrays), so a
// shared high byte would false-positive the 2-instruction pending test.
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)ITEM_PENDING_TAG),
                     "pending tag must be non-double");
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NOT_INLINE_INT((uint8_t)ITEM_PENDING_TAG),
                     "pending tag must stay out of the inline-int octant");
LAMBDA_STATIC_ASSERT(ITEM_PENDING_TAG > LMD_CONTAINER_HEAP_START,
                     "pending tag must sit above every semantic TypeId and heap marker");
LAMBDA_STATIC_ASSERT(ITEM_PENDING_TAG < LAMBDA_TAG_SPACE_SIZE,
                     "pending tag must stay inside the 000 octant tag space");
LAMBDA_STATIC_ASSERT(ITEM_PENDING_TAG != ITEM_SENTINEL_TAG,
                     "pending tag must stay distinct from the sentinel tag");
LAMBDA_STATIC_ASSERT((uint8_t)(ITEM_JS_DELETED_SENTINEL >> 56) != (uint8_t)ITEM_PENDING_TAG &&
                     (uint8_t)(ITEM_JS_ITER_DONE_SENTINEL >> 56) != (uint8_t)ITEM_PENDING_TAG &&
                     (uint8_t)(ITEM_JS_ITER_CLOSE_ABSENT_SENTINEL >> 56) != (uint8_t)ITEM_PENDING_TAG,
                     "storable JS sentinels must not share the pending high byte");
LAMBDA_STATIC_ASSERT(PENDING_KIND_FLOAT <= PENDING_KIND_MASK,
                     "pending kinds must fit the reserved low bits");
// The three wide-scalar tags are CONTIGUOUS, and the JIT's pending-pair builder
// depends on it: `(unsigned)(tag - LMD_TYPE_INT64) <= 2` is the whole fast-path
// test, and `tag - LMD_TYPE_INT64` IS the pending kind for the two integer
// tags. Reordering EnumTypeId without preserving this run silently
// mis-classifies returns, so pin it here.
LAMBDA_STATIC_ASSERT(LMD_TYPE_UINT64 == LMD_TYPE_INT64 + 1 &&
                     LMD_TYPE_FLOAT == LMD_TYPE_INT64 + 2,
                     "wide scalar tags must stay contiguous from LMD_TYPE_INT64");
LAMBDA_STATIC_ASSERT((uint64_t)(LMD_TYPE_INT64 - LMD_TYPE_INT64) == PENDING_KIND_INT64 &&
                     (uint64_t)(LMD_TYPE_UINT64 - LMD_TYPE_INT64) == PENDING_KIND_UINT64,
                     "integer pending kinds must equal their tag offset");
// Tags 0x06-0x08 have bits 6 and 5 clear, so an Item whose high byte lands in
// the wide-scalar run can never be an inline double. The pair builder relies on
// that to skip the ITEM_DBL_MASK test on the wide arm.
LAMBDA_STATIC_ASSERT(ITEM_TAG_IS_NON_DOUBLE((uint8_t)LMD_TYPE_INT64) &&
                     ITEM_TAG_IS_NON_DOUBLE((uint8_t)LMD_TYPE_FLOAT),
                     "wide scalar tags must be outside inline-double space");

static inline void lambda_item_debug_trap(void) {
#if defined(_MSC_VER)
    __debugbreak();
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_trap();
#else
    *((volatile int*)0) = 0;
#endif
}

// Tripwire for RV4.1: a pending Item must never reach a value accessor. It is
// a return-lane transport, resolved by the caller before any consume/store, so
// seeing one here means an emitter site skipped its resolution point.
static inline void assert_item_not_pending(uint64_t bits) {
#if !defined(NDEBUG)
    if (lambda_item_is_pending(bits)) {
        lambda_item_debug_trap();
    }
#else
    (void)bits;
#endif
}

static inline void assert_raw_item_pointer(const void* ptr) {
#if !defined(NDEBUG)
    if (!ptr) return;
    uint64_t word = (uint64_t)(uintptr_t)ptr;
    // Raw-pointer Items must be bit-identical native pointers, never tagged.
    if ((word & ITEM_HIGH_BYTE_MASK) != 0 || (word & ITEM_DBL_MASK) != 0 ||
        (const void*)(uintptr_t)word != ptr) {
        lambda_item_debug_trap();
    }
#else
    (void)ptr;
#endif
}

// v5 (`vibe/Lambda_Semantics_Int_Type.md` §5.4): `int` is the contiguous band
// +/-(2^53 - 1), and a FINITE int boxes as a 56-bit two's-complement payload
// under the LMD_TYPE_INT tag byte:
//
//     box(n)   = (LMD_TYPE_INT << 56) | (n & MASK56)     // AND + OR
//     unbox(i) = ((int64_t)(i << 8)) >> 8                // sign-extend low 56
//
// One instruction each way on aarch64 (bfi / sbfx), no FP unit on the int path.
// The payload has 56 bits and the band needs 54, so every int value round-trips
// exactly and the encoding is canonical.
//
// POISON IS NOT PACKED. `inf`/`-inf`/`nan` are the values `int` SHARES with
// `float` (formal semantics 4, kept by v5), stored as ordinary inline IEEE bits
// in the float octants. So a tag-byte-INT Item ALWAYS decodes to a finite band
// value -- the payload invariant every consumer may rely on.
//
// History: v4 (C16) instead carried the value's own IEEE bits rotated left by
// one, which put int in the `100` octant and made int/float share a native
// lane. That is retired with the double lane; see the design doc §1 for the
// v1-v5 arc and `Lambda_Type_Int_Boxing.md` for the rotation scheme itself.
#define ITEM_INT_PAYLOAD_MASK  UINT64_C(0x00FFFFFFFFFFFFFF)  // low 56 bits

// The band. Under v5 this is the WHOLE int domain -- not just the literal
// ingestion rule -- so it is simultaneously the carrier capacity, the
// saturation point, and the `int <= float` subtyping edge (§5.1: every band
// value converts to double exactly, which is why int53 beats int56).
#define INT53_MAX  ((int64_t)9007199254740991LL)   // +(2^53 - 1)
#define INT53_MIN  ((int64_t)-9007199254740991LL)  // -(2^53 - 1)

// Raw IEEE bit patterns for the three shared poison values.
#define LAMBDA_IEEE_INF_BITS      UINT64_C(0x7FF0000000000000)
#define LAMBDA_IEEE_NEG_INF_BITS  UINT64_C(0xFFF0000000000000)
#define LAMBDA_IEEE_NAN_BITS      UINT64_C(0x7FF8000000000000)

// LANE SENTINELS -- PRIVATE to the native i64 lane (§5.1). IEEE bits cannot
// live in an i64, so within the lane only, poison is three reserved i64 values.
// They are converted at box/unbox and NEVER escape the lane: no Item, no
// container, no guest bridge ever sees one.
//
// Placed at the two's-complement extremes for two reasons. (1) Degradation:
// they sit >= 2^63 - 2^54 from the band, so a sentinel that leaks past a
// forgotten check lands far out of band and re-poisons at the next band check
// instead of laundering into a finite value. (2) `neg`/`abs` become BRANCH-FREE
// total -- -(INT64_MIN) wraps to itself so nan stays nan, -(INT64_MAX) is
// INT64_MIN+1 so +inf becomes -inf, and the band is symmetric so every finite
// case closes. The classic two's-complement negation trap becomes the mechanism
// that propagates poison correctly.
#define INT_LANE_NAN      INT64_MIN
#define INT_LANE_NEG_INF  (INT64_MIN + 1)
#define INT_LANE_INF      INT64_MAX
// This fourth sentinel is valid only in an int? lane. It shares ItemNull's
// word so box/unbox is exact, but ArrayNum must demote before it is stored.
#define INT_LANE_NULL     ((int64_t)ITEM_NULL)
// Widened i8?…u32? lanes use the same out-of-domain bit pattern. Keep a
// separate name so sized-lane checks cannot accidentally be routed through
// int poison arithmetic.
#define SIZED_LANE_NULL   INT_LANE_NULL

// ItemNull is not an IEEE double bit-pattern, so float? reserves one quiet
// NaN payload in its native lane. Ordinary Lambda NaN uses the canonical
// zero-payload quiet NaN above; normalize this one payload on stores so a
// guest numeric NaN can never be mistaken for nullable absence.
#define FLOAT_LANE_NULL_BITS UINT64_C(0x7FF8000000000001)

static inline bool lambda_float_lane_is_null(uint64_t bits) {
    return bits == FLOAT_LANE_NULL_BITS;
}

static inline uint64_t lambda_float_lane_from_double(double value) {
    uint64_t bits;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    return bits == FLOAT_LANE_NULL_BITS ? LAMBDA_IEEE_NAN_BITS : bits;
}

static inline double lambda_float_lane_to_double(uint64_t bits) {
    double value;
    __builtin_memcpy(&value, &bits, sizeof(value));
    return value;
}

// An IEEE special: exponent all ones, so inf (mantissa 0) or nan.
#define LAMBDA_ITEM_IS_IEEE_SPECIAL(b) \
    (((b) & UINT64_C(0x7FF0000000000000)) == UINT64_C(0x7FF0000000000000))

// The merged poison values (inf, -inf, nan) are the ones `int` and `float`
// share. They are PHYSICALLY doubles, so get_type_id() must keep reporting
// FLOAT -- it is the decoder dispatch, and every site that reads an Item
// relies on it to pick the right lane. The C16 ruling that `type(nan) == int`
// is a SURFACE claim, applied in fn_type()/item_static_type_for_is() only.
// Conflating the two routes nan into integer decoders (LambdaJS has many
// number-coercion call sites, while hosted JS Symbols are pointer-backed).
static inline bool lambda_item_is_merged_poison(uint64_t bits) {
    return (bits & ITEM_DBL_MASK) && LAMBDA_ITEM_IS_IEEE_SPECIAL(bits);
}

// Does this Item word carry its whole value, with no pointer into heap, pool,
// or frame storage? Such a word stays valid for the life of the process, so it
// may outlive the frame that produced it and may be baked into cacheable MIR
// (DI14). This is a question about representation, not about TypeId: most
// doubles are self-tagged (the Item *is* the IEEE bit pattern) while the
// residue `lambda_float_ptr_to_item` cannot pack is a pointer to a boxed
// double. [RC4, vibe/Lambda_Design_Runtime_Const.md]
static inline bool lambda_item_is_self_contained(uint64_t bits) {
    // the double test comes first: an inline double can carry any high byte.
    if (bits & ITEM_DBL_MASK) return true;
    switch ((uint8_t)(bits >> 56)) {
    case LMD_TYPE_NULL:
    case LMD_TYPE_BOOL:
    case LMD_TYPE_INT:
        return true;
    case LMD_TYPE_FLOAT:
        // ±0 are packed outside the double space; every other float word here
        // failed to pack and holds a pointer.
        return bits == ITEM_FLOAT_P0 || bits == ITEM_FLOAT_N0;
    default:
        return false;
    }
}

// Is this Item word a packed (finite) int? The tag byte alone decides -- but
// the double test must come first, because an inline double can carry any high
// byte. Callers inside type_id() have already excluded doubles.
#define LAMBDA_ITEM_IS_PACKED_INT(item_bits) \
    ((((item_bits) & ITEM_HIGH_BYTE_MASK) >> 56) == (uint64_t)LMD_TYPE_INT)

// Box a LANE value as an int Item. TOTAL: the three sentinels become their
// shared IEEE values, and an out-of-band finite value SATURATES by sign rather
// than truncating (formal semantics 4.9 -- an int result that cannot be carried
// as an int is +/-inf, never a wrapped or error value). So `i2it` never fails,
// which is the O1-class safety property v4 established and v5 keeps.
static inline uint64_t lambda_int_box_lane(int64_t lane) {
    if (lane == INT_LANE_NULL) return ITEM_NULL;
    if (lane >= INT53_MIN && lane <= INT53_MAX) {
        return ITEM_INT | ((uint64_t)lane & ITEM_INT_PAYLOAD_MASK);
    }
    if (lane == INT_LANE_NAN) return LAMBDA_IEEE_NAN_BITS;
    // covers both +/-inf sentinels AND ordinary out-of-band saturation
    return lane > 0 ? LAMBDA_IEEE_INF_BITS : LAMBDA_IEEE_NEG_INF_BITS;
}

// Box a double known to be integral (or poison) as an int Item. The caller's
// contract is integrality; magnitude is handled here by saturation.
static inline uint64_t lambda_int_box_double(double value) {
    if (value != value) return LAMBDA_IEEE_NAN_BITS;
    if (value > (double)INT53_MAX) return LAMBDA_IEEE_INF_BITS;
    if (value < (double)INT53_MIN) return LAMBDA_IEEE_NEG_INF_BITS;
    return ITEM_INT | ((uint64_t)(int64_t)value & ITEM_INT_PAYLOAD_MASK);
}

// The LANE value of an int Item: finite payloads sign-extend, poison maps to
// its lane sentinel. This is the ONLY place an Item becomes a lane value.
static inline int64_t lambda_int_item_to_lane(uint64_t item_bits) {
    if (item_bits == ITEM_NULL) return INT_LANE_NULL;
    if (item_bits & ITEM_DBL_MASK) {  // shared poison, inline IEEE
        if ((item_bits & UINT64_C(0x000FFFFFFFFFFFFF)) != 0) return INT_LANE_NAN;
        return (item_bits >> 63) ? INT_LANE_NEG_INF : INT_LANE_INF;
    }
    return ((int64_t)(item_bits << 8)) >> 8;  // sign-extend the 56-bit payload
}

// The value of an int Item as a double. Poison comes back as the IEEE special
// it denotes, so numeric consumers need no poison branch.
// The value of an int Item as a double. Poison comes back as the IEEE special
// it denotes, so numeric consumers need no poison branch. Exact for every
// finite value: the band is chosen precisely so that int -> double never
// rounds (§5.1 -- this is what `int <= float` rests on).
static inline double lambda_int_unbox_double(uint64_t item_bits) {
    double result;
    if (item_bits & ITEM_DBL_MASK) {  // shared inf/nan, stored as raw IEEE bits
        __builtin_memcpy(&result, &item_bits, sizeof(result));
        return result;
    }
    return (double)(((int64_t)(item_bits << 8)) >> 8);
}

// Lane <-> double conversions. Header-only because `core/` uses them and must
// not link `runtime/`; the runtime keeps thin `_c` wrappers so the JIT registry
// has a callable symbol.
static inline double lambda_int_lane_to_double(int64_t lane) {
    return lambda_int_unbox_double(lambda_int_box_lane(lane));
}

static inline int64_t lambda_double_to_int_lane(double value) {
    if (value != value) return INT_LANE_NAN;
    if (value > (double)INT53_MAX) return INT_LANE_INF;
    if (value < (double)INT53_MIN) return INT_LANE_NEG_INF;
    return (int64_t)value;
}


// (The retired int-poison classifiers lived here. With one shared inf/nan
// representation, "is this poison?" is lambda_item_is_merged_poison() and
// nan-ness survives every unbox, so no sentinel-aware predicate is needed.)

// Classify an int *value* held as a double. Kept as the value-side spelling of
// the same question so numeric code can ask before it re-boxes.
#define LAMBDA_INT_VALUE_IS_POISON(v)  (!((v) == (v)) || (v) > 1.7976931348623157e308 || (v) < -1.7976931348623157e308)
#define LAMBDA_INT_VALUE_IS_NAN(v)     (!((v) == (v)))
#define LAMBDA_INT_VALUE_IS_INF(v)     ((v) > 1.7976931348623157e308)
#define LAMBDA_INT_VALUE_IS_NEG_INF(v) ((v) < -1.7976931348623157e308)

// Unresolved lazy global. Unlike the sentinels above it is stored in ordinary
// property storage, which round-trips a scalar through its *value*, so this
// marker has to be a real int value ("JUBELZ") rather than a magic payload on
// some tag — a payload would be re-encoded to a different word on the way back.
#define ITEM_JS_LAZY_GLOBAL_MAGIC     ((int64_t)0x004A5542454C5A)
#define ITEM_JS_LAZY_GLOBAL_SENTINEL  i2it(ITEM_JS_LAZY_GLOBAL_MAGIC)

static inline uint64_t lambda_int64_ptr_to_item_bits(const int64_t* ptr) {
    if (!ptr) return ITEM_NULL;
    uint64_t payload = (uint64_t)(uintptr_t)ptr;
    if (payload & ITEM_HIGH_BYTE_MASK) return ITEM_ERROR;
    return ITEM_INT64 | payload;
}

static inline uint64_t lambda_uint64_ptr_to_item_bits(const uint64_t* ptr) {
    if (!ptr) return ITEM_NULL;
    uint64_t payload = (uint64_t)(uintptr_t)ptr;
    if (payload & ITEM_HIGH_BYTE_MASK) return ITEM_ERROR;
    return ((uint64_t)LMD_TYPE_UINT64 << 56) | payload;
}

inline uint64_t b2it(uint8_t bool_val) {
    return bool_val >= BOOL_ERROR ? ITEM_ERROR : ((((uint64_t)LMD_TYPE_BOOL)<<56) | bool_val);
}
// Box an int64 as an `int` Item. v5 closes the finite domain at int53, so
// out-of-band input saturates to the shared IEEE infinity instead of wrapping,
// silently rounding, or producing an error Item.
#define i2it(int_val)        lambda_int_box_lane((int64_t)(int_val))
// BigInt: same as decimal tagged pointer (Decimal::storage_kind == DECIMAL_BIGINT)
#define bi2it(decimal_ptr)   c2it(decimal_ptr)
#define l2it(long_ptr)       lambda_int64_ptr_to_item_bits((const int64_t*)(long_ptr))
#define d2it(double_ptr)     ((double_ptr)? ((((uint64_t)LMD_TYPE_FLOAT)<<56) | (uint64_t)(double_ptr)): ITEM_NULL)

#ifdef __cplusplus
static inline Item lambda_float_ptr_to_item(const double* double_ptr);
#else
static inline Item lambda_float_ptr_to_item(const double* double_ptr) {
    if (!double_ptr) return ITEM_NULL;
    double value = *double_ptr;
    uint64_t bits;
    __builtin_memcpy(&bits, &value, sizeof(bits));
    if (value == 0.0) return ITEM_FLOAT_P0 | ((bits >> 63) ? UINT64_C(1) : UINT64_C(0));
    if (bits & ITEM_DBL_MASK) return bits;
    return d2it(double_ptr);
}
#endif
#define c2it(decimal_ptr)    ((decimal_ptr)? ((((uint64_t)LMD_TYPE_DECIMAL)<<56) | (uint64_t)(decimal_ptr)): ITEM_NULL)
#define s2it(str_ptr)        ((str_ptr)? ((((uint64_t)LMD_TYPE_STRING)<<56) | (uint64_t)(str_ptr)): ITEM_NULL)
#define y2it(sym_ptr)        ((sym_ptr)? ((((uint64_t)LMD_TYPE_SYMBOL)<<56) | (uint64_t)(sym_ptr)): ITEM_NULL)
#define x2it(bin_ptr)        ((bin_ptr)? ((((uint64_t)LMD_TYPE_BINARY)<<56) | (uint64_t)(bin_ptr)): ITEM_NULL)
#define k2it(dtime_ptr)      ((dtime_ptr)? ((((uint64_t)LMD_TYPE_DTIME)<<56) | (uint64_t)(dtime_ptr)): ITEM_NULL)
#define u2it(uint64_ptr)     lambda_uint64_ptr_to_item_bits((const uint64_t*)(uint64_ptr))

// Float16/Float32 packing into NUM_SIZED Items
// float32: store IEEE 754 binary32 bit pattern in low 32 bits
// Use compiler builtins for type-safe bit conversion.
static inline uint32_t f32_to_bits(float f) { uint32_t u; __builtin_memcpy(&u, &f, 4); return u; }
static inline float bits_to_f32(uint32_t u) { float f; __builtin_memcpy(&f, &u, 4); return f; }
#define f32_to_item(v) NUM_SIZED_PACK(NUM_FLOAT32, f32_to_bits((float)(v)))

// float16: software conversion (IEEE 754 binary16)
static inline uint16_t f32_to_f16_bits(float f) {
    uint32_t b = f32_to_bits(f);
    uint32_t sign = (b >> 16) & 0x8000;
    int32_t  expo = ((b >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = (b >> 13) & 0x03FF;
    if (expo <= 0) return (uint16_t)sign;         // underflow → ±0
    if (expo >= 31) return (uint16_t)(sign | 0x7C00); // overflow → ±inf
    return (uint16_t)(sign | (expo << 10) | mant);
}
static inline float f16_bits_to_f32(uint16_t h) {
    uint32_t sign = ((uint32_t)h & 0x8000) << 16;
    uint32_t expo = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x03FF;
    if (expo == 0) { if (mant == 0) return bits_to_f32(sign); /* denorm → 0 */ }
    if (expo == 31) return bits_to_f32(sign | 0x7F800000 | (mant << 13)); // inf/nan
    uint32_t result = sign | ((expo - 15 + 127) << 23) | (mant << 13);
    return bits_to_f32(result);
}
#define f16_to_item(v) NUM_SIZED_PACK(NUM_FLOAT16, (uint32_t)f32_to_f16_bits((float)(v)))

// Unpack sized numeric value from Item
static inline int8_t   item_to_i8(uint64_t it)  { return (int8_t)(NUM_SIZED_RAW32(it) & 0xFF); }
static inline int16_t  item_to_i16(uint64_t it) { return (int16_t)(NUM_SIZED_RAW32(it) & 0xFFFF); }
static inline int32_t  item_to_i32(uint64_t it) { return (int32_t)NUM_SIZED_RAW32(it); }
static inline uint8_t  item_to_u8(uint64_t it)  { return (uint8_t)(NUM_SIZED_RAW32(it) & 0xFF); }
static inline uint16_t item_to_u16(uint64_t it) { return (uint16_t)(NUM_SIZED_RAW32(it) & 0xFFFF); }
static inline uint32_t item_to_u32(uint64_t it) { return NUM_SIZED_RAW32(it); }
static inline float    item_to_f32(uint64_t it) { return bits_to_f32(NUM_SIZED_RAW32(it)); }
static inline float    item_to_f16(uint64_t it) { return f16_bits_to_f32((uint16_t)(NUM_SIZED_RAW32(it) & 0xFFFF)); }

// Nullable sized-integer lanes widen to i64 so no source-domain bit pattern
// is reserved. Floating-sized values have their own IEEE representation and
// are intentionally not admitted by this integer-lane helper.
static inline bool lambda_num_sized_is_integer(NumSizedType num_type) {
    return num_type >= NUM_INT8 && num_type <= NUM_UINT32;
}

static inline int64_t lambda_num_sized_item_to_lane(uint64_t item) {
    switch ((NumSizedType)NUM_SIZED_SUBTYPE(item)) {
    case NUM_INT8: return (int64_t)item_to_i8(item);
    case NUM_INT16: return (int64_t)item_to_i16(item);
    case NUM_INT32: return (int64_t)item_to_i32(item);
    case NUM_UINT8: return (int64_t)item_to_u8(item);
    case NUM_UINT16: return (int64_t)item_to_u16(item);
    case NUM_UINT32: return (int64_t)item_to_u32(item);
    default: return 0;
    }
}

// ============================================================================
// Forward declaration for structured error handling
// ============================================================================
typedef struct LambdaError LambdaError;

// ============================================================================
// Container unboxing helpers: Item → native container pointer
// C mode casts the Item word directly; C++ helpers follow Item in core/value.hpp.
// ============================================================================

#ifndef __cplusplus
// Container unboxing: Item → native pointer (simple cast).
// Container Items store direct pointers (no type tag in the high bits),
// so no masking is needed — just cast to the target pointer type.
#define it2map(item)    ((Map*)(uintptr_t)(item))
#define it2list(item)   ((List*)(uintptr_t)(item))
#define it2elmt(item)   ((Element*)(uintptr_t)(item))
#define it2obj(item)    ((Object*)(uintptr_t)(item))
#define it2arr(item)    ((Array*)(uintptr_t)(item))
#define it2range(item)  ((Range*)(uintptr_t)(item))
#define it2path(item)   ((Path*)(uintptr_t)(item))
#define it2p(item)      ((void*)(uintptr_t)(item))

// Container boxing helper: native pointer → Item
static inline Item p2it(void* ptr) {
    if (!ptr) return ITEM_NULL;
    assert_raw_item_pointer(ptr);
    return (Item)(uint64_t)(uintptr_t)ptr;
}

// Convert LambdaError* → Error Item (LMD_TYPE_ERROR-tagged pointer)
static inline Item err2it(LambdaError* err) {
    if (!err) return ITEM_NULL;
    return (Item)(((uint64_t)LMD_TYPE_ERROR << 56) | (uint64_t)(uintptr_t)err);
}

// Convert Error Item → LambdaError* (extract pointer from tagged Item)
static inline LambdaError* it2err(Item item) {
    uint8_t tag = (uint64_t)item >> 56;
    if (tag != LMD_TYPE_ERROR) return null;
    return (LambdaError*)(uintptr_t)((uint64_t)item & 0x00FFFFFFFFFFFFFFULL);
}
#endif // !__cplusplus

// C helpers use the same merged-lane tag test as the C++ runtime.  Keep the
// implementation here so native host boundaries do not need the C++ Item API.
#ifndef __cplusplus
static inline bool item_is_error(Item item) {
    return ((uint64_t)item >> 56) == LMD_TYPE_ERROR;
}

// Error completion from a LambdaError* that may be null (payload allocation
// failed): a null payload still completes as the pointer-less ERROR sentinel,
// never as null (D1.4). This is the one spelling of "return this error".
static inline Item err2it_or_error(LambdaError* err) {
    return err ? err2it(err) : ITEM_ERROR;
}
#endif // !__cplusplus

// descriptor-aware packed slots; the caller keeps the map/value owners rooted.
struct ShapeEntry;
#ifdef __cplusplus
extern "C" {
#endif
void set_field_value(struct ShapeEntry* field, void* field_ptr, Item item);
Item map_shape_field_to_item(void* map_data, const struct ShapeEntry* field);
#ifdef __cplusplus
}
#endif

// A shaped field is stored in the lane the store path chose for it, which is
// NOT always ShapeEntry::type->type_id: a non-simple `type` contract (`T?`, a
// union, a constrained or occurrence type) carries type_id LMD_TYPE_TYPE yet
// lands in the TypedItem `any` lane or a container-pointer lane. The packed
// layout is an ABI (D3.4.1), so a C consumer that decodes it -- the GC's shape
// walk above all -- reads the descriptor the entry stores (SCU9/D3.4.6): its
// kind, nullability and decoding TypeId, never Type::type_id.
#ifdef __cplusplus
extern "C" {
#endif
void lambda_shape_entry_lane(const void* shape_entry, uint8_t* kind,
    uint8_t* nullable, uint8_t* value_domain);
// JS accessor cells are ShapeEntry-owned virtual edges. The collector uses
// this bridge instead of duplicating the trailing C++ ShapeEntry layout.
void* lambda_shape_entry_accessor(const void* shape_entry);
uint64_t lambda_shape_nominal_owner(const void* type_map);
#ifdef __cplusplus
}
#endif

typedef struct Url Url;
typedef struct Pool Pool;
typedef struct Arena Arena;

// Forward declaration of ArrayList (defined in lib/arraylist.h)
typedef struct _ArrayList ArrayList;

// Scalar and packed-field operations provided by the input data model.
#ifdef __cplusplus
extern "C" {
#endif
bool map_field_store_dynamic_item(void* field_ptr, Item value);
bool item_try_to_int64(Item item, int64_t* out);
bool item_try_to_double(Item item, double* out);
LAMBDA_CORE_API int64_t it2l(Item item);
uint64_t it2u(Item item);
LAMBDA_CORE_API double it2d(Item item);
LAMBDA_CORE_API bool it2b(Item item);
LAMBDA_CORE_API int64_t it2i(Item item);
DateTime* it2k(Item item);
LAMBDA_CORE_API String* it2s(Item item);
Binary* it2x(Item item);
const char* fn_to_cstr(Item item);  // convert Item to C string (for path segment names)
#ifdef __cplusplus
}
#endif

#endif // !LAMBDA_API_ONLY && !LAMBDA_MAIN_HEADER_INCLUDED

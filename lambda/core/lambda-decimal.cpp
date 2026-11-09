// lambda/lambda-decimal.cpp - Centralized decimal handling for Lambda
// =====================================================================
// Decimal representation uses libmpdec here; runtime operations live in Lambda.
// Other modules use the API declared in lambda-decimal.hpp.

#include "lambda-decimal.hpp"
#include "data.hpp"
#include "lib/log.h"
#include "lib/mem.h"
#include "lib/str.h"
#include "lib/strbuf.h"
#include "lib/arraylist.h"
#include <mpdecimal.h>  // only included here
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ─────────────────────────────────────────────────────────────────────
// Global Contexts
// ─────────────────────────────────────────────────────────────────────

static mpd_context_t g_fixed_ctx;      // 34-digit decimal128 precision
static mpd_context_t g_unlimited_ctx;  // extended inexact-operation context
static bool g_initialized = false;

// ─────────────────────────────────────────────────────────────────────
// Context Management
// ─────────────────────────────────────────────────────────────────────

// Scale binary64 coordinates by 2^1074 into exact integers. The degree-four
// incircle determinant needs at most 4*2099+4 bits, including differences,
// squared lifts, and six signed terms. 30103/100000 bounds log10(2) above.
bool decimal_geometry_sign(const double* points, int count, int dimension,
        bool lifted, int* sign) {
    if (!points || !sign || !((count == 3 && dimension == 2 && !lifted) ||
            (count == 4 && dimension == (lifted ? 2 : 3)))) return false;
    for (int i = 0; i < count * dimension; i++) if (!isfinite(points[i])) return false;
    mpd_context_t ctx;
    mpd_maxcontext(&ctx);
    const int bits = 4 * (DBL_MAX_EXP - DBL_MIN_EXP + DBL_MANT_DIG + 1) + 4;
    ctx.prec = (bits * 30103 + 99999) / 100000 + 1;
    ctx.traps = 0;
    uint32_t status = 0;
    mpd_t* storage[26] = {NULL};
    for (int i = 0; i < 26; i++) {
        storage[i] = mpd_new(&ctx);
        if (!storage[i]) {
            for (int j = 0; j < i; j++) mpd_del(storage[j]);
            return false;
        }
    }
    mpd_t** coords = storage;
    mpd_t** matrix = storage + 12;
    mpd_t* two = storage[21];
    mpd_t* exponent = storage[22];
    mpd_t* work = storage[23];
    mpd_t* product = storage[24];
    mpd_t* result = storage[25];
    mpd_qset_i32(two, 2, &ctx, &status);
    for (int i = 0; i < count * dimension; i++) {
        uint64_t raw;
        memcpy(&raw, &points[i], sizeof(raw));
        int exp = (int)((raw >> 52) & 0x7ff);
        uint64_t mantissa = raw & UINT64_C(0x000fffffffffffff);
        if (exp) mantissa |= UINT64_C(0x0010000000000000);
        mpd_qset_u64(coords[i], mantissa, &ctx, &status);
        mpd_qset_i32(exponent, exp ? exp - 1 : 0, &ctx, &status);
        mpd_qpow(work, two, exponent, &ctx, &status);
        mpd_qmul(coords[i], coords[i], work, &ctx, &status);
        if (raw >> 63) mpd_qcopy_negate(coords[i], coords[i], &status);
    }
    int rank = count - 1;
    for (int row = 0; row < rank; row++) {
        for (int col = 0; col < dimension; col++)
            mpd_qsub(matrix[row * 3 + col], coords[row * dimension + col],
                coords[rank * dimension + col], &ctx, &status);
        if (lifted) {
            mpd_qmul(matrix[row * 3 + 2], matrix[row * 3], matrix[row * 3], &ctx, &status);
            mpd_qmul(work, matrix[row * 3 + 1], matrix[row * 3 + 1], &ctx, &status);
            mpd_qadd(matrix[row * 3 + 2], matrix[row * 3 + 2], work, &ctx, &status);
        }
    }
    mpd_qset_i32(result, 0, &ctx, &status);
    int permutation[] = {0, 1, 2};
    while (true) {
        mpd_qset_i32(product, 1, &ctx, &status);
        int inversions = 0;
        for (int row = 0; row < rank; row++) {
            mpd_qmul(product, product, matrix[row * 3 + permutation[row]], &ctx, &status);
            for (int j = row + 1; j < rank; j++) inversions += permutation[row] > permutation[j];
        }
        if (inversions & 1) mpd_qsub(result, result, product, &ctx, &status);
        else mpd_qadd(result, result, product, &ctx, &status);
        int k = rank - 2;
        while (k >= 0 && permutation[k] > permutation[k + 1]) k--;
        if (k < 0) break;
        int j = rank - 1;
        while (permutation[j] < permutation[k]) j--;
        int t = permutation[k]; permutation[k] = permutation[j]; permutation[j] = t;
        for (int lo = k + 1, hi = rank - 1; lo < hi; lo++, hi--) {
            t = permutation[lo]; permutation[lo] = permutation[hi]; permutation[hi] = t;
        }
    }
    bool ok = !(status & (MPD_Errors | MPD_Inexact | MPD_Overflow | MPD_Underflow));
    if (ok) *sign = mpd_iszero(result) ? 0 : mpd_isnegative(result) ? -1 : 1;
    for (int i = 0; i < 26; i++) mpd_del(storage[i]);
    return ok;
}

void decimal_init() {
    if (g_initialized) return;
    
    // Initialize fixed-precision context (decimal128 precision)
    mpd_defaultcontext(&g_fixed_ctx);
    g_fixed_ctx.prec = DECIMAL_FIXED_PRECISION;
    // Lambda keeps numeric poison in the value domain; mpdecimal traps would
    // abort before operations such as 0m * decimal.inf can produce decimal.nan.
    g_fixed_ctx.traps = 0;
    
    // Initialize the context for inexact operations on the extended tier.
    // Exact operations use a local max-precision context so storage can grow.
    mpd_maxcontext(&g_unlimited_ctx);
    g_unlimited_ctx.prec = 200;
    g_unlimited_ctx.traps = 0;
    
    g_initialized = true;
    log_debug("decimal_init: fixed_prec=%d, extended_prec=%d",
              (int)g_fixed_ctx.prec, (int)g_unlimited_ctx.prec);
}

void decimal_cleanup() {
    // Nothing to cleanup for static contexts
    g_initialized = false;
}

mpd_context_t* decimal_fixed_context() {
    if (!g_initialized) decimal_init();
    return &g_fixed_ctx;
}

mpd_context_t* decimal_unlimited_context() {
    if (!g_initialized) decimal_init();
    return &g_unlimited_ctx;
}

bool lambda_finite_double_to_fixed(double value, int digits, bool half_up, char* out, int out_size) {
    if (!isfinite(value) || digits < 0 || digits > 100 || !out || out_size <= 0) return false;
    bool negative = value < 0;
    double magnitude = fabs(value);
    int length = snprintf(out, out_size, "%s%.*f", negative ? "-" : "", digits, magnitude);
    if (length < 0 || length >= out_size) return false;
    if (!half_up || !magnitude) return true;
    // x = odd * 2^e: x*10^digits is an exact half only when e+digits == -1.
    // 5^digits == 1 (mod 4), so odd == 1 (mod 4) identifies ties rounded down to even.
    int exponent;
    uint64_t odd = (uint64_t)ldexp(frexp(magnitude, &exponent), 53);
    exponent -= 53;
    while (!(odd & 1)) { odd >>= 1; exponent++; }
    if (exponent + digits != -1 || (odd & 3) != 1) return true;
    for (int i = length - 1; i >= (int)negative; i--) {
        if (out[i] == '.') continue;
        if (out[i] != '9') { out[i]++; return true; }
        out[i] = '0';
    }
    if (length + 1 >= out_size) return false;
    memmove(out + negative + 1, out + negative, length - negative + 1);
    out[negative] = '1';
    return true;
}

void lambda_finite_double_to_shortest(double d, char* out, int out_size) {
    if (!out || out_size <= 0) return;
    if (d == 0.0) {
        snprintf(out, out_size, "0");
        return;
    }

    bool neg = false;
    if (d < 0) {
        neg = true;
        d = -d;
    }

    // T10-0/D-A: integral magnitudes below 2^53 are their own shortest form —
    // adjacent doubles there are at least 1 apart, so no shorter decimal can
    // round-trip, and ES Number::toString emits the exact integer digits (k<=e<=21).
    // Array indices and most JS numbers land here; the generic path below cost up
    // to 21 snprintf/sscanf round trips per `a[i]` key conversion.
    if (d < 9007199254740992.0 && d == (double)(int64_t)d) {
        char buf[24];
        uint64_t v = (uint64_t)(int64_t)d;
        size_t length = str_uint64_decimal_write(buf, v);
        char* o = out;
        char* end = out + out_size - 1;
        if (neg && o < end) *o++ = '-';
        for (size_t i = 0; i < length && o < end; i++) *o++ = buf[i];
        *o = '\0';
        return;
    }

    char sci[64];
    int best_len = 0;
    // A binary64 round-trip never needs more than DBL_DECIMAL_DIG
    // significant digits. For a normal double, every decimal of at most
    // DBL_DIG (15) digits also survives the trip, so rounding d to 15 digits
    // yields its shortest spelling whenever that is <= 15 digits (padded with
    // zeros, stripped below). Probing 15, 16, 17 thus spells exactly what
    // probing 1..17 did (S4.7.1) in at most three tries instead of up to
    // seventeen snprintf/sscanf pairs; strtod parses back like sscanf("%lf").
    // A subnormal carries fewer significant bits, so its 15-digit rounding can
    // round-trip while a shorter spelling exists (5e-324 would come out as
    // 4.94065645841247e-324): below DBL_MIN, probe from one digit as before.
    // Checked identical to the 1..17 probe on 20M random doubles.
    int first_prec = d < DBL_MIN ? 1 : DBL_DIG;
    for (int prec = first_prec; prec <= DBL_DECIMAL_DIG; prec++) {
        snprintf(sci, sizeof(sci), "%.*e", prec - 1, d);
        double roundtrip = strtod(sci, NULL);
        if (roundtrip == d) {
            best_len = prec;
            break;
        }
    }
    // sci already holds the spelling at best_len: the loop either broke on
    // it or ended on the DBL_DECIMAL_DIG probe, which is the fallback length
    if (best_len == 0) best_len = DBL_DECIMAL_DIG;

    char digits[32];
    int digit_count = 0;
    int exp_val = 0;
    char* p = sci;
    if (*p == '-') p++;
    while (*p && *p != 'e' && *p != 'E') {
        if (*p != '.') {
            digits[digit_count++] = *p;
        }
        p++;
    }
    digits[digit_count] = '\0';
    while (digit_count > 1 && digits[digit_count - 1] == '0') {
        digit_count--;
        digits[digit_count] = '\0';
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        exp_val = atoi(p);
    }

    int k = digit_count;
    int e = exp_val + 1;
    char* o = out;
    char* end = out + out_size - 1;
    if (neg && o < end) *o++ = '-';

    if (k <= e && e <= 21) {
        for (int i = 0; i < k && o < end; i++) *o++ = digits[i];
        for (int i = 0; i < e - k && o < end; i++) *o++ = '0';
        *o = '\0';
    } else if (0 < e && e <= 21) {
        for (int i = 0; i < e && o < end; i++) *o++ = digits[i];
        if (o < end) *o++ = '.';
        for (int i = e; i < k && o < end; i++) *o++ = digits[i];
        *o = '\0';
    } else if (-6 < e && e <= 0) {
        if (o < end) *o++ = '0';
        if (o < end) *o++ = '.';
        for (int i = 0; i < -e && o < end; i++) *o++ = '0';
        for (int i = 0; i < k && o < end; i++) *o++ = digits[i];
        *o = '\0';
    } else if (k == 1) {
        if (o < end) *o++ = digits[0];
        if (o < end) *o++ = 'e';
        if (e - 1 >= 0 && o < end) *o++ = '+';
        snprintf(o, (size_t)(end - o + 1), "%d", e - 1);
    } else {
        if (o < end) *o++ = digits[0];
        if (o < end) *o++ = '.';
        for (int i = 1; i < k && o < end; i++) *o++ = digits[i];
        if (o < end) *o++ = 'e';
        if (e - 1 >= 0 && o < end) *o++ = '+';
        snprintf(o, (size_t)(end - o + 1), "%d", e - 1);
    }
}

void lambda_double_to_shortest(double d, char* out, int out_size) {
    if (!out || out_size <= 0) return;
    if (isnan(d)) {
        snprintf(out, out_size, "nan");
        return;
    }
    if (isinf(d)) {
        snprintf(out, out_size, "%sinf", d < 0 ? "-" : "");
        return;
    }
    lambda_finite_double_to_shortest(d, out, out_size);
}

bool lambda_numeric_to_canonical_string(Item item, char* out, int out_size) {
    if (!out || out_size <= 0) return false;
    out[0] = '\0';
    TypeId type = get_type_id(item);
    // Complex is numeric in Lambda but has no scalar decimal canonicalization.
    if (type == LMD_TYPE_COMPLEX || !IS_NUMERIC_ID(type)) return false;

    if (type == LMD_TYPE_FLOAT || type == LMD_TYPE_INT) {
        // `int` and `float` share `inf`/`nan` and one native form, so the
        // canonical string -- which is what hashing is built on -- must treat
        // them identically. Omitting int here gave an int-typed `inf` a
        // different hash from `decimal.inf` while `==` called them equal, so a
        // map keyed by both grew two entries: hashing has to respect equality
        // (P9).
        double val = type == LMD_TYPE_INT ? lambda_int_item_value(item)
                                          : item.get_double();
        if (isnan(val)) return false;
        if (isinf(val)) {
            snprintf(out, out_size, "%sinf", val < 0 ? "-" : "");
            return true;
        }
    } else if (type == LMD_TYPE_NUM_SIZED) {
        double val = item.get_num_sized_as_double();
        if ((item.get_num_type() == NUM_FLOAT16 || item.get_num_type() == NUM_FLOAT32) &&
            isnan(val)) {
            return false;
        }
        if ((item.get_num_type() == NUM_FLOAT16 || item.get_num_type() == NUM_FLOAT32) &&
            isinf(val)) {
            snprintf(out, out_size, "%sinf", val < 0 ? "-" : "");
            return true;
        }
    } else if (type == LMD_TYPE_DECIMAL) {
        Decimal* decimal = item.get_decimal();
        const mpd_t* value = decimal_mpd(decimal);
        if (!value || mpd_isnan(value)) return false;
        if (mpd_isinfinite(value)) {
            snprintf(out, out_size, "%sinf", mpd_isnegative(value) ? "-" : "");
            return true;
        }
    }

    mpd_context_t* ctx = decimal_unlimited_context();
    mpd_t* dec = decimal_item_to_mpd(item, ctx);
    if (!dec) return false;

    mpd_t* reduced = mpd_new(ctx);
    if (!reduced) {
        mpd_del(dec);
        return false;
    }

    // Equal decimal values must hash the same even when their source spelling differs.
    uint32_t status = 0;
    mpd_qreduce(reduced, dec, ctx, &status);
    mpd_t* canon = (status == 0) ? reduced : dec;
    char* str = mpd_to_sci(canon, 1);
    if (!str) {
        mpd_del(reduced);
        mpd_del(dec);
        return false;
    }
    snprintf(out, out_size, "%s", str);
    mpd_free(str);
    mpd_del(reduced);
    mpd_del(dec);
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// Parsing
// ─────────────────────────────────────────────────────────────────────

mpd_t* decimal_parse_str(const char* str, mpd_context_t* ctx) {
    if (!str || !ctx) return NULL;
    
    mpd_t* dec_val = mpd_new(ctx);
    if (!dec_val) {
        log_error("decimal_parse_str: failed to allocate mpd_t");
        return NULL;
    }
    
    uint32_t status = 0;
    mpd_qset_string(dec_val, str, ctx, &status);
    
    if (status != 0) {
        log_error("decimal_parse_str: failed to parse '%s' (status: %u)", str, status);
        mpd_del(dec_val);
        return NULL;
    }
    
    return dec_val;
}

mpd_t* decimal_parse_str_exact(const char* str) {
    if (!str) return NULL;

    mpd_t* dec_val = mpd_new(decimal_unlimited_context());
    if (!dec_val) {
        log_error("decimal_parse_str_exact: failed to allocate mpd_t");
        return NULL;
    }

    uint32_t status = 0;
    mpd_qset_string_exact(dec_val, str, &status);
    if (status != 0) {
        log_error("decimal_parse_str_exact: failed to parse '%s' (status: %u)", str, status);
        mpd_del(dec_val);
        return NULL;
    }
    return dec_val;
}

mpd_t* decimal_parse_fixed_str(const char* str) {
    return decimal_parse_str(str, decimal_fixed_context());
}

mpd_t* decimal_parse_unlimited_str(const char* str) {
    return decimal_parse_str_exact(str);
}

bool decimal_take_mpd(Decimal* decimal, DecimalKind storage_kind, mpd_t* mpd_val) {
    if (!decimal || !mpd_val) {
        if (mpd_val) mpd_del(mpd_val);
        return false;
    }

    // mpd_del() honours MPD_STATIC: transfer the library-owned header into
    // Decimal, retain its coefficient allocation, and make the wrapper's
    // address non-freeable when the GC releases that coefficient later.
    decimal->storage_kind = storage_kind;
    decimal->dec_val = *mpd_val;
    mpd_set_static(&decimal->dec_val);
    mpd_free(mpd_val);
    return true;
}


void decimal_free_string(char* str) {
    if (str) mpd_free(str);
}

// Deep copy a decimal Item (for arena allocation in MarkBuilder)
#include "lib/arena.h"
extern mpd_context_t* InputManager_decimal_context();  // forward declare

Item decimal_deep_copy(Item item, void* arena_ptr) {
    if (!decimal_is_any(item)) return ItemNull;
    
    Decimal* src_dec = item.get_decimal();
    if (!decimal_has_payload(src_dec)) return ItemNull;
    
    Arena* arena = (Arena*)arena_ptr;
    mpd_context_t* ctx = decimal_fixed_context();  // use global context
    
    // Create new mpd_t and copy the value
    mpd_t* new_dec_val = mpd_new(ctx);
    if (!new_dec_val) return ItemNull;
    
    // Copy using qcopy
    uint32_t status = 0;
    mpd_qcopy(new_dec_val, decimal_mpd(src_dec), &status);
    if (status != 0) {
        mpd_del(new_dec_val);
        return ItemNull;
    }
    
    // Allocate Decimal struct in arena
    Decimal* new_dec = (Decimal*)arena_alloc(arena, sizeof(Decimal));
    if (!new_dec) {
        mpd_del(new_dec_val);
        return ItemNull;
    }
    
    if (!decimal_take_mpd(new_dec, src_dec->storage_kind, new_dec_val)) return ItemNull;
    
    Item result;
    result.item = c2it(new_dec);
    return result;
}

static Item decimal_from_string_arena_with_context(const char* str, void* arena_ptr,
                                                   mpd_context_t* ctx, DecimalKind storage_kind) {
    if (!str || !arena_ptr) return ItemNull;

    Arena* arena = (Arena*)arena_ptr;
    mpd_t* dec_val = mpd_new(ctx);
    if (!dec_val) return ItemNull;

    uint32_t status = 0;
    // input values and literals must not be rounded merely because their
    // coefficient exceeds the fixed-tier precision.
    mpd_qset_string_exact(dec_val, str, &status);
    if (status != 0 || mpd_isnan(dec_val) || mpd_isinfinite(dec_val)) {
        mpd_del(dec_val);
        return ItemNull;
    }

    Decimal* dec = (Decimal*)arena_alloc(arena, sizeof(Decimal));
    if (!dec) {
        mpd_del(dec_val);
        return ItemNull;
    }

    if (!decimal_take_mpd(dec, storage_kind, dec_val)) return ItemNull;

    Item result;
    result.item = c2it(dec);
    return result;
}

// Create a decimal Item from a string, arena-allocated.
// The Decimal struct lives in the arena; the mpd_t* is malloc'd by mpdecimal
// (not GC-managed). Safe to use from input parsers where GC heap allocation
// would cause the object to be collected before it can be traced.

Item decimal_from_string_arena(const char* str, void* arena_ptr) {
    DecimalKind storage_kind = str_decimal_significant_digits(str) >
        DECIMAL_FIXED_PRECISION ? DECIMAL_EXTENDED : DECIMAL_FIXED;
    return decimal_from_string_arena_with_context(str, arena_ptr,
        decimal_fixed_context(), storage_kind);
}

Item decimal_from_literal_string_arena(const char* str, void* arena_ptr, bool is_integer_literal) {
    if (!str || !arena_ptr) return ItemNull;

    bool needs_unlimited = is_integer_literal ||
        str_decimal_significant_digits(str) > DECIMAL_FIXED_PRECISION;
    mpd_context_t* ctx = needs_unlimited ? decimal_unlimited_context() : decimal_fixed_context();
    DecimalKind storage_kind = is_integer_literal ? DECIMAL_BIGINT :
        (needs_unlimited ? DECIMAL_EXTENDED : DECIMAL_FIXED);
    return decimal_from_string_arena_with_context(str, arena_ptr, ctx, storage_kind);
}

Item decimal_from_integer_string_arena(const char* str, void* arena_ptr) {
    return decimal_from_string_arena_with_context(str, arena_ptr,
        decimal_unlimited_context(), DECIMAL_EXTENDED);
}

// Create a fixed-precision Decimal from a double, arena-allocated.
Item decimal_from_double_arena(double val, void* arena_ptr) {
    if (!arena_ptr) return ItemNull;
    
    Arena* arena = (Arena*)arena_ptr;
    mpd_context_t* ctx = decimal_fixed_context();
    mpd_t* dec_val = mpd_new(ctx);
    if (!dec_val) return ItemNull;
    
    char str_buf[64];
    lambda_double_to_shortest(val, str_buf, sizeof(str_buf));
    
    uint32_t status = 0;
    mpd_qset_string(dec_val, str_buf, ctx, &status);
    if (status != 0) {
        mpd_del(dec_val);
        return ItemNull;
    }
    
    Decimal* dec = (Decimal*)arena_alloc(arena, sizeof(Decimal));
    if (!dec) {
        mpd_del(dec_val);
        return ItemNull;
    }
    
    if (!decimal_take_mpd(dec, DECIMAL_FIXED, dec_val)) return ItemNull;
    
    Item result;
    result.item = c2it(dec);
    return result;
}

// Create a fixed-precision Decimal from an int64, arena-allocated.
Item decimal_from_int64_arena(int64_t val, void* arena_ptr) {
    if (!arena_ptr) return ItemNull;
    
    Arena* arena = (Arena*)arena_ptr;
    mpd_context_t* ctx = decimal_fixed_context();
    mpd_t* dec_val = mpd_new(ctx);
    if (!dec_val) return ItemNull;
    
    mpd_set_ssize(dec_val, (mpd_ssize_t)val, ctx);
    
    Decimal* dec = (Decimal*)arena_alloc(arena, sizeof(Decimal));
    if (!dec) {
        mpd_del(dec_val);
        return ItemNull;
    }
    
    if (!decimal_take_mpd(dec, DECIMAL_FIXED, dec_val)) return ItemNull;
    
    Item result;
    result.item = c2it(dec);
    return result;
}

// ─────────────────────────────────────────────────────────────────────
// Formatting
// ─────────────────────────────────────────────────────────────────────

void decimal_print(StrBuf* strbuf, Decimal* decimal) {
    const mpd_t* value = decimal_mpd(decimal);
    if (!value) {
        strbuf_append_str(strbuf, "error");
        return;
    }
    
    const char* special = decimal_special_literal(decimal);
    if (special) {
        strbuf_append_str(strbuf, special);
        return;
    }

    // Use libmpdec to format finite values with no truncation.
    char* decimal_str = mpd_to_sci(value, 1);  // scientific notation
    if (!decimal_str) {
        strbuf_append_str(strbuf, "error");
        return;
    }
    
    strbuf_append_str(strbuf, decimal_str);
    mpd_free(decimal_str);
}

void decimal_big_print(StrBuf* strbuf, Decimal* decimal) {
    // Same implementation - no truncation for either type
    decimal_print(strbuf, decimal);
}


// ─────────────────────────────────────────────────────────────────────
// Type Conversion
// ─────────────────────────────────────────────────────────────────────

mpd_t* decimal_item_to_mpd(Item item, mpd_context_t* ctx) {
    // Decimal Items are heap pointers; use the resolved type, not the raw tag.
    TypeId type = get_type_id(item);
    
    // If already a decimal, make a copy
    if (type == LMD_TYPE_DECIMAL) {
        Decimal* dec_ptr = item.get_decimal();
        if (!decimal_has_payload(dec_ptr)) return NULL;
        
        mpd_t* copy = mpd_new(ctx);
        if (!copy) return NULL;
        mpd_copy(copy, decimal_mpd(dec_ptr), ctx);
        return copy;
    }
    
    mpd_t* result = mpd_new(ctx);
    if (!result) return NULL;
    
    // S4.7.1: mixed decimal arithmetic uses the one shortest round-trip
    // spelling for every native float conversion.
    if (type == LMD_TYPE_INT) {
        // C16/G0: `int`'s native form is the IEEE double, and it shares `inf`
        // and `nan` with `float`. An i64 conversion cannot carry either, nor a
        // magnitude past 2^63 -- it turned `decimal.inf == inf` false, breaking
        // the rule that same-signed infinities are one value across every
        // poison-bearing domain. Route through the same shortest-string path
        // `float` uses, which mpd parses including the specials.
        double val = lambda_int_item_value(item);
        char str_buf[64];
        lambda_double_to_shortest(val, str_buf, sizeof(str_buf));
        uint32_t status = 0;
        mpd_qset_string(result, str_buf, ctx, &status);
        if (status != 0) {
            mpd_del(result);
            return NULL;
        }
    }
    else if (type == LMD_TYPE_INT64) {
        mpd_set_ssize(result, item.get_int64(), ctx);
    }
    else if (type == LMD_TYPE_UINT64) {
        mpd_set_u64(result, item.get_uint64(), ctx);
    }
    else if (type == LMD_TYPE_NUM_SIZED) {
        if (item.get_num_type() == NUM_FLOAT16 || item.get_num_type() == NUM_FLOAT32) {
            double val = item.get_num_sized_as_double();
            char str_buf[64];
            lambda_double_to_shortest(val, str_buf, sizeof(str_buf));
            uint32_t status = 0;
            mpd_qset_string(result, str_buf, ctx, &status);
            if (status != 0) {
                mpd_del(result);
                return NULL;
            }
        } else {
            mpd_set_ssize(result, item.get_num_sized_as_int64(), ctx);
        }
    }
    else if (type == LMD_TYPE_FLOAT) {
        double val = item.get_double();
        char str_buf[64];
        lambda_double_to_shortest(val, str_buf, sizeof(str_buf));
        uint32_t status = 0;
        mpd_qset_string(result, str_buf, ctx, &status);
        if (status != 0) {
            mpd_del(result);
            return NULL;
        }
    }
    else {
        mpd_del(result);
        return NULL;
    }
    
    return result;
}

bool decimal_mpd_try_to_int64(mpd_t* dec, mpd_context_t* ctx, int64_t* out) {
    (void)ctx;
    if (!dec || !out) return false;

    // mpd_get_ssize raises SIGFPE when a value exceeds the host-sized range.
    uint32_t status = 0;
    mpd_ssize_t result = mpd_qget_ssize(dec, &status);
    if (status & MPD_Invalid_operation) return false;
    *out = (int64_t)result;
    return true;
}

bool decimal_mpd_try_to_double(mpd_t* dec, mpd_context_t* ctx, double* out) {
    (void)ctx;
    if (!dec || !out) return false;

    if (mpd_isnan(dec)) {
        *out = mpd_isnegative(dec) ? -NAN : NAN;
        return true;
    }
    if (mpd_isinfinite(dec)) {
        *out = mpd_isnegative(dec) ? -INFINITY : INFINITY;
        return true;
    }

    // Integral decimal values in the common machine range do not need a
    // temporary scientific spelling.
    if (mpd_isinteger(dec)) {
        uint32_t status = 0;
        int64_t integral = mpd_qget_i64(dec, &status);
        if ((status & MPD_Invalid_operation) == 0) {
            *out = (double)integral;
            if (integral == 0 && mpd_isnegative(dec)) *out = -0.0;
            return true;
        }
    }

    char* str = mpd_to_sci(dec, 1);
    if (!str) return false;
    char* end = NULL;
    double result = strtod(str, &end);
    bool complete = end && end != str && *end == '\0';
    mpd_free(str);
    if (!complete) return false;
    *out = result;
    return true;
}

double decimal_mpd_to_double(mpd_t* dec, mpd_context_t* ctx) {
    double result = 0.0;
    if (!decimal_mpd_try_to_double(dec, ctx, &result)) return 0.0;
    return result;
}

// ─────────────────────────────────────────────────────────────────────
// Predicates
// ─────────────────────────────────────────────────────────────────────

bool decimal_is_zero(mpd_t* dec) {
    return dec && mpd_iszero(dec);
}

bool decimal_is_unlimited(Item item) {
    if (item._type_id != LMD_TYPE_DECIMAL) return false;
    Decimal* dec_ptr = item.get_decimal();
    return dec_ptr && dec_ptr->storage_kind != DECIMAL_FIXED;
}

bool decimal_is_any(Item item) {
    return item._type_id == LMD_TYPE_DECIMAL;
}


// ─────────────────────────────────────────────────────────────────────
// Comparison
// ─────────────────────────────────────────────────────────────────────

static bool decimal_cmp(Item a, Item b, mpd_context_t* ctx, int* result) {
    if (!result) return false;
    bool a_is_dec = decimal_is_any(a);
    bool b_is_dec = decimal_is_any(b);

    Decimal* a_decimal = a_is_dec ? a.get_decimal() : NULL;
    Decimal* b_decimal = b_is_dec ? b.get_decimal() : NULL;
    mpd_t* a_dec = a_is_dec ? decimal_mpd(a_decimal) :
        decimal_item_to_mpd(a, ctx);
    mpd_t* b_dec = b_is_dec ? decimal_mpd(b_decimal) :
        decimal_item_to_mpd(b, ctx);
    
    if (!a_dec || !b_dec) {
        if (!a_is_dec && a_dec) mpd_del(a_dec);
        if (!b_is_dec && b_dec) mpd_del(b_dec);
        return false;
    }

    *result = mpd_cmp(a_dec, b_dec, ctx);
    
    if (!a_is_dec) mpd_del(a_dec);
    if (!b_is_dec) mpd_del(b_dec);
    
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// Item-level Comparison (no mpd_context_t* in signature)
// ─────────────────────────────────────────────────────────────────────

bool decimal_cmp_items(Item a, Item b, int* result) {
    mpd_context_t* ctx = decimal_is_unlimited(a) || decimal_is_unlimited(b) 
        ? decimal_unlimited_context() 
        : decimal_fixed_context();
    return decimal_cmp(a, b, ctx, result);
}

// ─────────────────────────────────────────────────────────────────────
// Item-level Predicates  
// ─────────────────────────────────────────────────────────────────────

bool decimal_item_is_zero(Item item) {
    if (!decimal_is_any(item)) return false;
    Decimal* dec_ptr = item.get_decimal();
    if (!decimal_has_payload(dec_ptr)) return false;
    return mpd_iszero(decimal_mpd(dec_ptr));
}

bool decimal_item_is_nan(Item item) {
    if (!decimal_is_any(item)) return false;
    Decimal* decimal = item.get_decimal();
    return decimal_has_payload(decimal) && mpd_isnan(decimal_mpd(decimal));
}

bool decimal_item_is_infinite(Item item) {
    if (!decimal_is_any(item)) return false;
    Decimal* decimal = item.get_decimal();
    return decimal_has_payload(decimal) && mpd_isinfinite(decimal_mpd(decimal));
}

const char* decimal_special_literal(Decimal* decimal) {
    const mpd_t* value = decimal_mpd(decimal);
    if (!value) return NULL;
    if (mpd_isnan(value)) return "decimal.nan";
    if (!mpd_isinfinite(value)) return NULL;
    return mpd_isnegative(value) ? "-decimal.inf" : "decimal.inf";
}

// ─────────────────────────────────────────────────────────────────────
// Conversion helpers
// ─────────────────────────────────────────────────────────────────────

bool decimal_try_to_double(Item item, double* out) {
    if (!decimal_is_any(item) || !out) return false;
    Decimal* dec_ptr = item.get_decimal();
    if (!decimal_has_payload(dec_ptr)) return false;
    return decimal_mpd_try_to_double(decimal_mpd(dec_ptr), NULL, out);
}

double decimal_to_double(Item item) {
    double result = 0.0;
    if (!decimal_try_to_double(item, &result)) return 0.0;
    return result;
}

char* decimal_to_string(Item item) {
    if (!decimal_is_any(item)) return NULL;
    Decimal* dec_ptr = item.get_decimal();
    if (!decimal_has_payload(dec_ptr)) return NULL;
    return mpd_to_sci(decimal_mpd(dec_ptr), 1);  // caller must free with decimal_free_string
}

char* decimal_to_string(Decimal* decimal) {
    if (!decimal_has_payload(decimal)) return NULL;
    return mpd_to_sci(decimal_mpd(decimal), 1);  // caller must free with decimal_free_string
}


// Convert decimal Item to int64 (truncates toward zero).
bool decimal_try_to_int64(Item item, int64_t* out) {
    if (!out || !decimal_is_any(item)) return false;
    Decimal* dec_ptr = item.get_decimal();
    if (!decimal_has_payload(dec_ptr)) return false;
    
    mpd_context_t* dec_ctx = dec_ptr->storage_kind != DECIMAL_FIXED ?
        decimal_unlimited_context() : decimal_fixed_context();
    
    // truncate first, then convert
    mpd_t* truncated = mpd_new(dec_ctx);
    if (!truncated) return false;
    mpd_trunc(truncated, decimal_mpd(dec_ptr), dec_ctx);
    bool converted = decimal_mpd_try_to_int64(truncated, dec_ctx, out);
    mpd_del(truncated);
    return converted;
}

bool decimal_to_int64_exact(Item item, int64_t* out) {
    if (!out || !decimal_is_any(item)) return false;
    Decimal* decimal = item.get_decimal();
    const mpd_t* value = decimal_mpd(decimal);
    if (!value || mpd_isnan(value) || mpd_isinfinite(value) || !mpd_isinteger(value)) return false;

    // Semantic `integer` now reaches ordinary integral consumers after mixed
    // full-width arithmetic; reject out-of-range values instead of inheriting
    // bigint_to_int64's intentional clamping behavior.
    uint32_t status = 0;
    mpd_ssize_t int_value = mpd_qget_ssize(value, &status);
    if (status & MPD_Invalid_operation) return false;
    *out = (int64_t)int_value;
    return true;
}

bool decimal_to_uint64_exact(Item item, uint64_t* out) {
    if (!out || !decimal_is_any(item)) return false;
    Decimal* decimal = item.get_decimal();
    const mpd_t* value = decimal_mpd(decimal);
    if (!value || mpd_isnan(value) || mpd_isinfinite(value) || !mpd_isinteger(value) ||
            mpd_isnegative(value)) return false;

    // The boundary admission path must reject an out-of-range decimal before a
    // native unsigned cast; mpdecimal reports that case instead of wrapping it.
    uint32_t status = 0;
    uint64_t uint_value = mpd_qget_u64(value, &status);
    if (status != 0) return false;
    *out = uint_value;
    return true;
}

#include <gtest/gtest.h>
#include "lambda/core/data.hpp"

static int host_calls;
static TypeId observed_type;
static uint64_t observed_bits;

static Item scalar_host(Item value, bool js_number) {
    EXPECT_FALSE(js_number);
    observed_type = get_type_id(value);
    observed_bits = observed_type == LMD_TYPE_INT64 ? (uint64_t)value.get_int64() : value.get_uint64();
    host_calls++;
    return {.item = i2it(42)};
}

TEST(RuntimeScalarStorage, WideArrayReadsRequireBoundProvider) {
    ArrayNum array = {};
    array.type_id = LMD_TYPE_ARRAY_NUM;
    array.length = 1;
    int64_t signed_value = INT64_MAX;
    array.items = &signed_value;
    array.set_elem_type(ELEM_INT64);
    scalar_storage_bind_host(nullptr);
    EXPECT_EQ(array_num_read_borrowed_item(&array, 0).item, ItemError.item);

    uint64_t unsigned_value = UINT64_MAX;
    array.data = &unsigned_value;
    array.set_elem_type(ELEM_UINT64);
    EXPECT_EQ(array_num_read_borrowed_item(&array, 0).item, ItemError.item);
}

TEST(RuntimeScalarStorage, WideArrayReadsUseBoundProvider) {
    ArrayNum array = {};
    array.type_id = LMD_TYPE_ARRAY_NUM;
    array.length = 1;
    host_calls = 0;
    scalar_storage_bind_host(scalar_host);
    int64_t signed_value = INT64_MAX;
    array.items = &signed_value;
    array.set_elem_type(ELEM_INT64);
    EXPECT_EQ(array_num_read_borrowed_item(&array, 0).item, i2it(42));
    EXPECT_EQ(observed_type, LMD_TYPE_INT64);
    EXPECT_EQ(observed_bits, (uint64_t)signed_value);

    uint64_t unsigned_value = UINT64_MAX;
    array.data = &unsigned_value;
    array.set_elem_type(ELEM_UINT64);
    EXPECT_EQ(array_num_read_borrowed_item(&array, 0).item, i2it(42));
    EXPECT_EQ(observed_type, LMD_TYPE_UINT64);
    EXPECT_EQ(observed_bits, unsigned_value);
    EXPECT_EQ(host_calls, 2);
    scalar_storage_bind_host(nullptr);
}

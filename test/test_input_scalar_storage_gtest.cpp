#include <gtest/gtest.h>
#include "lambda/core/data.hpp"
#include <pthread.h>

static __thread int scalar_host_calls;

static Item test_scalar_host(Item item, bool js_number) {
    (void)item;
    scalar_host_calls++;
    return {.item = i2it(js_number ? 22 : 11)};
}

struct ScalarHostGuard {
    ScalarHostGuard() { scalar_storage_bind_host(test_scalar_host); }
    ~ScalarHostGuard() { scalar_storage_bind_host(nullptr); }
};

TEST(InputScalarStorage, NativeReaderBorrowsFromItsInputOwner) {
    int64_t value = INT64_MAX;
    Item borrowed = {.item = l2it(&value)};
    Item read = scalar_storage_read(borrowed, false);
    EXPECT_EQ(read.item, borrowed.item);
    EXPECT_EQ(read.get_int64(), INT64_MAX);
}

TEST(InputScalarStorage, RuntimeHostReceivesOnlyEscapingScalarReads) {
    ScalarHostGuard host;
    scalar_host_calls = 0;
    int64_t value = INT64_MAX;
    Item borrowed = {.item = l2it(&value)};
    EXPECT_EQ(scalar_storage_read(borrowed, true).item, borrowed.item);
    EXPECT_EQ(scalar_storage_read({.item = i2it(3)}, false).item, i2it(3));
    EXPECT_EQ(scalar_host_calls, 0);
    EXPECT_EQ(scalar_storage_read(borrowed, false).item, i2it(11));
    EXPECT_EQ(js_scalar_storage_read(borrowed, false).item, i2it(22));
    EXPECT_EQ(scalar_host_calls, 2);
}

TEST(InputScalarStorage, WideArrayReadsKeepInputOwnedStorageWithHostBound) {
    ScalarHostGuard host;
    scalar_host_calls = 0;
    ArrayNum array = {};
    array.type_id = LMD_TYPE_ARRAY_NUM;
    array.length = 1;

    int64_t signed_value = INT64_MAX;
    array.set_elem_type(ELEM_INT64);
    array.items = &signed_value;
    Item signed_read = array_num_read_borrowed_item(&array, 0);
    EXPECT_EQ(signed_read.item, l2it(&signed_value));
    EXPECT_EQ(signed_read.get_int64(), signed_value);

    uint64_t unsigned_value = UINT64_MAX;
    array.set_elem_type(ELEM_UINT64);
    array.data = &unsigned_value;
    Item unsigned_read = array_num_read_borrowed_item(&array, 0);
    EXPECT_EQ(unsigned_read.item, u2it(&unsigned_value));
    EXPECT_EQ(unsigned_read.get_uint64(), unsigned_value);
    EXPECT_EQ(scalar_host_calls, 0);
}

static void* read_without_thread_host(void* argument) {
    Item* value = (Item*)argument;
    *value = scalar_storage_read(*value, false);
    return nullptr;
}

TEST(InputScalarStorage, HostBindingDoesNotCrossThreads) {
    ScalarHostGuard host;
    int64_t value = INT64_MAX;
    Item borrowed = {.item = l2it(&value)};
    Item worker_value = borrowed;
    pthread_t worker;
    ASSERT_EQ(pthread_create(&worker, nullptr, read_without_thread_host, &worker_value), 0);
    ASSERT_EQ(pthread_join(worker, nullptr), 0);
    EXPECT_EQ(worker_value.item, borrowed.item);
    EXPECT_EQ(scalar_storage_read(borrowed, false).item, i2it(11));
}

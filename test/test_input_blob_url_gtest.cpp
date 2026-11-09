#include <gtest/gtest.h>
#include "lambda/input/css/dom_blob_url.hpp"
#include "lambda/input/css/dom_element.hpp"
#include "lambda/input/input.hpp"
#include "lib/mem.h"

TEST(InputBlobUrlTest, DocumentOwnershipAndRevocation) {
    Pool* pool = pool_create();
    ASSERT_NE(pool, nullptr);
    Input* input = Input::create(pool);
    ASSERT_NE(input, nullptr);
    DomDocument* first = dom_document_create(input);
    DomDocument* second = dom_document_create(input);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const char* first_url = dom_blob_url_create(first, mem_strdup("data:text/plain;base64,YQ==", MEM_CAT_SYSTEM));
    const char* second_url = dom_blob_url_create(second, mem_strdup("data:text/plain;base64,Yg==", MEM_CAT_SYSTEM));
    ASSERT_NE(first_url, nullptr);
    ASSERT_NE(second_url, nullptr);
    char* first_copy = mem_strdup(first_url, MEM_CAT_SYSTEM);
    ASSERT_NE(first_copy, nullptr);
    EXPECT_STRNE(first_url, second_url);
    EXPECT_STREQ(dom_blob_url_data_uri(first, first_url), "data:text/plain;base64,YQ==");
    EXPECT_EQ(dom_blob_url_data_uri(second, first_url), nullptr);
    dom_blob_url_revoke(first, first_copy);
    EXPECT_EQ(dom_blob_url_data_uri(first, first_copy), nullptr);
    EXPECT_STREQ(dom_blob_url_data_uri(second, second_url), "data:text/plain;base64,Yg==");
    dom_document_destroy(first);
    EXPECT_STREQ(dom_blob_url_data_uri(second, second_url), "data:text/plain;base64,Yg==");
    dom_document_destroy(second);
    mem_free(first_copy);
    pool_destroy(pool);
}

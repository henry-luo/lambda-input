#include <gtest/gtest.h>
#include "lib/https_pull.h"
#include "lib/mem.h"
#include <stdlib.h>
#include <string.h>

TEST(InputOwnedHttpsPull, TrustedFixtureAndProductionLoopbackPolicy) {
    const char* url = getenv("INPUT_CURL_FIXTURE_URL");
    const char* ca = getenv("INPUT_CURL_FIXTURE_CA");
    const char* wrong_ca = getenv("INPUT_CURL_FIXTURE_WRONG_CA");
    const char* resolve = getenv("INPUT_CURL_FIXTURE_RESOLVE");
    ASSERT_NE(url, nullptr); ASSERT_NE(ca, nullptr);
    ASSERT_NE(wrong_ca, nullptr); ASSERT_NE(resolve, nullptr);
    HttpsPullRequest request = {url, 1024, 5000, nullptr, nullptr};
    HttpsPullFixture fixture = {resolve, ca, true};
    HttpsPullResponse response = {};
    ASSERT_EQ(https_pull_fixture(&request, &fixture, &response), HTTPS_PULL_OK);
    EXPECT_EQ(response.http_status, 200u);
    ASSERT_EQ(response.size, strlen("input-owned curl"));
    EXPECT_EQ(memcmp(response.bytes, "input-owned curl", response.size), 0);
    mem_free(response.bytes);

    fixture.ca_file = wrong_ca;
    response = {};
    EXPECT_EQ(https_pull_fixture(&request, &fixture, &response), HTTPS_PULL_TLS_ERROR);
    EXPECT_EQ(response.bytes, nullptr);
    mem_free(response.bytes);

    // The test-only loopback permission must not reach the production entry point.
    response = {};
    EXPECT_EQ(https_pull_once(&request, &response), HTTPS_PULL_FORBIDDEN_PEER);
    EXPECT_EQ(response.bytes, nullptr);
    mem_free(response.bytes);
}

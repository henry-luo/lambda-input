#include <gtest/gtest.h>
#include "lib/curl_runtime.h"
#include "lib/curl_trust.h"
#include <stdlib.h>
#include <string.h>

struct InputCurlResponse {
    char bytes[32];
    size_t length;
};

static size_t input_curl_write(char* data, size_t size, size_t count, void* context) {
    InputCurlResponse* response = (InputCurlResponse*)context;
    size_t length = size * count;
    if (length > sizeof(response->bytes) - response->length) return 0;
    memcpy(response->bytes + response->length, data, length);
    response->length += length;
    return length;
}

static CURLcode input_curl_request(const char* url, const char* ca_path,
        InputCurlResponse* response, long* http_status) {
    mbedtls_x509_crt roots;
    mbedtls_x509_crt_init(&roots);
    if (ca_path && mbedtls_x509_crt_parse_file(&roots, ca_path) != 0) {
        mbedtls_x509_crt_free(&roots);
        return CURLE_SSL_CACERT_BADFILE;
    }
    CURL* curl = curl_easy_init();
    if (!curl) { mbedtls_x509_crt_free(&roots); return CURLE_FAILED_INIT; }
    // Native downloads use the host trust adapter; private pulls provide explicit roots.
    CURLcode result = CURLE_OK;
    if (ca_path) result = curl_use_explicit_trust_store(curl, &roots);
    else curl_use_host_trust_store(curl);
    if (result == CURLE_OK) result = curl_easy_setopt(curl, CURLOPT_URL, url);
    if (result == CURLE_OK) result = curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    if (result == CURLE_OK) result = curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    if (result == CURLE_OK) result = curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, input_curl_write);
    if (result == CURLE_OK) result = curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    if (result == CURLE_OK) result = curl_easy_perform(curl);
    if (result == CURLE_OK) result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_status);
    curl_easy_cleanup(curl);
    mbedtls_x509_crt_free(&roots);
    return result;
}

TEST(InputOwnedCurl, UsesMbedTlsWithAsyncDnsAndHttp2) {
    ASSERT_TRUE(curl_runtime_initialize());
    const curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    ASSERT_NE(version, nullptr);
    ASSERT_NE(version->ssl_version, nullptr);
    EXPECT_EQ(strncmp(version->ssl_version, "mbedTLS/", 8), 0);
    EXPECT_NE(version->features & CURL_VERSION_ASYNCHDNS, 0);
    EXPECT_NE(version->features & CURL_VERSION_HTTP2, 0);
}

TEST(InputOwnedCurl, VerifiesTheConfiguredAuthorityAndServerName) {
    ASSERT_TRUE(curl_runtime_initialize());
    const char* url = getenv("INPUT_CURL_FIXTURE_URL");
    const char* ip_url = getenv("INPUT_CURL_FIXTURE_IP_URL");
    const char* ca = getenv("INPUT_CURL_FIXTURE_CA");
    const char* wrong_ca = getenv("INPUT_CURL_FIXTURE_WRONG_CA");
    ASSERT_NE(url, nullptr); ASSERT_NE(ip_url, nullptr);
    ASSERT_NE(ca, nullptr); ASSERT_NE(wrong_ca, nullptr);
    InputCurlResponse response = {};
    long status = 0;
    EXPECT_EQ(input_curl_request(url, ca, &response, &status), CURLE_OK);
    EXPECT_EQ(status, 200);
    EXPECT_EQ(response.length, strlen("input-owned curl"));
    EXPECT_EQ(memcmp(response.bytes, "input-owned curl", response.length), 0);
    response = {}; status = 0;
    EXPECT_EQ(input_curl_request(url, nullptr, &response, &status), CURLE_OK);
    EXPECT_EQ(status, 200);
    response = {}; status = 0;
    EXPECT_EQ(input_curl_request(url, wrong_ca, &response, &status), CURLE_PEER_FAILED_VERIFICATION);
    response = {}; status = 0;
    EXPECT_EQ(input_curl_request(ip_url, ca, &response, &status), CURLE_PEER_FAILED_VERIFICATION);
}

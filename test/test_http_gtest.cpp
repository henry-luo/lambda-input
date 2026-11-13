#include <gtest/gtest.h>
#include "lambda/input/input.hpp"
#include "lambda/network/http_client.h"
#include "lambda/network/cookie_jar.h"
#include "lib/url.h"
#include "lib/string.h"
#include "lib/mempool.h"
#include "lib/log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct HttpHeaderCaptureServer {
    pid_t pid;
    int request_fd;
    int port;
} HttpHeaderCaptureServer;

static bool http_header_capture_server_start(HttpHeaderCaptureServer* server,
                                             unsigned int response_delay_seconds = 0,
                                             const char* response_override = NULL,
                                             size_t response_override_size = 0) {
    if (!server) return false;
    memset(server, 0, sizeof(*server));
    server->request_fd = -1;

    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return false;
    int reuse_address = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                   &reuse_address, sizeof(reuse_address)) != 0) {
        close(listener);
        return false;
    }
    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr*)&address, sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        close(listener);
        return false;
    }
    socklen_t address_size = sizeof(address);
    if (getsockname(listener, (struct sockaddr*)&address, &address_size) != 0) {
        close(listener);
        return false;
    }

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        close(listener);
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(listener);
        return false;
    }
    if (pid == 0) {
        close(pipe_fds[0]);
        int client = accept(listener, NULL, NULL);
        char request[8192] = {};
        size_t request_size = 0;
        while (client >= 0 && request_size + 1 < sizeof(request)) {
            ssize_t read_size = recv(client, request + request_size,
                                     sizeof(request) - request_size - 1, 0);
            if (read_size <= 0) break;
            request_size += (size_t)read_size;
            if (strstr(request, "\r\n\r\n")) break;
        }
        if (request_size > 0) write(pipe_fds[1], request, request_size);
        const char default_response[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        const char* response = response_override ? response_override : default_response;
        size_t response_size = response_override ? response_override_size : sizeof(default_response) - 1;
        if (client >= 0) {
            if (response_delay_seconds > 0) sleep(response_delay_seconds);
            send(client, response, response_size, 0);
            close(client);
        }
        close(pipe_fds[1]);
        close(listener);
        _exit(0);
    }

    close(pipe_fds[1]);
    close(listener);
    server->pid = pid;
    server->request_fd = pipe_fds[0];
    server->port = ntohs(address.sin_port);
    return true;
}

static void http_header_capture_server_finish(HttpHeaderCaptureServer* server,
                                              char* request, size_t request_capacity) {
    ssize_t request_size = read(server->request_fd, request, request_capacity - 1);
    close(server->request_fd);
    int status = 0;
    ASSERT_EQ(server->pid, waitpid(server->pid, &status, 0));
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(0, WEXITSTATUS(status));
    ASSERT_GT(request_size, 0);
    request[request_size] = '\0';
}

class HttpInputTest : public ::testing::Test {
protected:
    Pool* pool;

    void SetUp() override {
        // Initialize logging
        log_init(NULL);
        // Create memory pool for tests
        pool = pool_create();
        ASSERT_NE(pool, nullptr) << "Failed to create memory pool";
    }

    void TearDown() override {
        // Cleanup memory pool
        if (pool) {
            pool_destroy(pool);
        }
    }
};

TEST_F(HttpInputTest, EmptyPostDoesNotReadStdin) {
    const char* bodies[] = {nullptr, ""};
    for (const char* body : bodies) {
        SCOPED_TRACE(body ? "explicit empty body" : "omitted body");
        HttpHeaderCaptureServer server;
        ASSERT_TRUE(http_header_capture_server_start(&server));
        char url[128];
        int length = snprintf(url, sizeof(url), "http://127.0.0.1:%d/empty-post", server.port);
        ASSERT_GT(length, 0);
        ASSERT_LT((size_t)length, sizeof(url));

        int input_pipe[2];
        ASSERT_EQ(pipe(input_pipe), 0);
        pid_t client = fork();
        ASSERT_GE(client, 0);
        if (client == 0) {
            // keep the writer open: an accidental stdin read must fail this test, not see EOF.
            alarm(3);
            if (dup2(input_pipe[0], STDIN_FILENO) < 0) _exit(2);
            close(input_pipe[0]);
            FetchConfig config = {};
            config.method = "POST";
            config.body = body;
            config.timeout_seconds = 2;
            config.user_agent = "Lambda HTTP regression";
            FetchResponse* response = http_fetch(url, &config);
            bool valid = response && response->status_code == 200 &&
                response->size == 2 && memcmp(response->data, "ok", 2) == 0;
            free_fetch_response(response);
            close(input_pipe[1]);
            _exit(valid ? 0 : 1);
        }
        close(input_pipe[0]);
        close(input_pipe[1]);
        int status = 0;
        ASSERT_EQ(waitpid(client, &status, 0), client);
        EXPECT_TRUE(WIFEXITED(status)) << "empty POST blocked on stdin";
        if (WIFEXITED(status)) EXPECT_EQ(WEXITSTATUS(status), 0);

        char request[8192] = {};
        http_header_capture_server_finish(&server, request, sizeof(request));
        EXPECT_NE(strstr(request, "POST /empty-post HTTP/1.1\r\n"), nullptr);
        EXPECT_NE(strstr(request, "Content-Length: 0\r\n"), nullptr);
        EXPECT_EQ(strstr(request, "Transfer-Encoding: chunked"), nullptr);
    }
}

TEST_F(HttpInputTest, SendsBrowserNavigationHeadersForDocumentDownloads) {
    HttpHeaderCaptureServer server;
    ASSERT_TRUE(http_header_capture_server_start(&server));

    char url[128];
    int url_size = snprintf(url, sizeof(url), "http://127.0.0.1:%d/document", server.port);
    ASSERT_GT(url_size, 0);
    ASSERT_LT(url_size, (int)sizeof(url));

    size_t content_size = 0;
    char* content = download_http_content(url, &content_size, NULL);

    char request[8192] = {};
    http_header_capture_server_finish(&server, request, sizeof(request));

    ASSERT_NE(nullptr, content);
    EXPECT_EQ(2u, content_size);
    EXPECT_STREQ("ok", content);
    EXPECT_NE(nullptr, strstr(request, "User-Agent: " RADIANT_HTTP_CLIENT_USER_AGENT));
    EXPECT_NE(nullptr, strstr(request, RADIANT_HTTP_DOCUMENT_ACCEPT_HEADER));
    EXPECT_NE(nullptr, strstr(request, RADIANT_HTTP_DOCUMENT_LANGUAGE_HEADER));
    EXPECT_NE(nullptr, strstr(request, RADIANT_HTTP_DOCUMENT_FETCH_DEST_HEADER));
    EXPECT_NE(nullptr, strstr(request, RADIANT_HTTP_DOCUMENT_FETCH_MODE_HEADER));
    EXPECT_NE(nullptr, strstr(request, RADIANT_HTTP_DOCUMENT_FETCH_SITE_HEADER));
    EXPECT_NE(nullptr, strstr(request, RADIANT_HTTP_DOCUMENT_FETCH_USER_HEADER));
    EXPECT_NE(nullptr, strstr(request, RADIANT_HTTP_DOCUMENT_UPGRADE_HEADER));
    EXPECT_EQ(nullptr, strstr(request, "Radiant/1.0"));
    mem_free(content);
}

TEST_F(HttpInputTest, KeepsRedirectCookiesWithoutSessionJar) {
    HttpHeaderCaptureServer destination;
    ASSERT_TRUE(http_header_capture_server_start(&destination));
    char redirect_response[512];
    int response_size = snprintf(redirect_response, sizeof(redirect_response),
        "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d/document\r\n"
        "Set-Cookie: navigation=allowed; Path=/\r\nContent-Length: 0\r\n\r\n",
        destination.port);
    ASSERT_GT(response_size, 0);
    ASSERT_LT((size_t)response_size, sizeof(redirect_response));
    HttpHeaderCaptureServer source;
    ASSERT_TRUE(http_header_capture_server_start(
        &source, 0, redirect_response, (size_t)response_size));
    char url[128];
    int url_size = snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", source.port);
    ASSERT_GT(url_size, 0);
    ASSERT_LT((size_t)url_size, sizeof(url));
    size_t content_size = 0;
    char* content = download_http_content_with_cookie_jar(url, &content_size, NULL);
    char request[8192] = {};
    http_header_capture_server_finish(&source, request, sizeof(request));
    http_header_capture_server_finish(&destination, request, sizeof(request));
    ASSERT_NE(content, nullptr);
    EXPECT_EQ(2u, content_size);
    EXPECT_STREQ(content, "ok");
    EXPECT_NE(strstr(request, "Cookie: navigation=allowed"), nullptr) << request;
    mem_free(content);
}

TEST_F(HttpInputTest, DecodesBrotliDocumentResponses) {
    static const char response[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Encoding: br\r\n"
        "Content-Length: 25\r\n"
        "\r\n"
        "\x21\x4c\x00\x04\x62\x72\x6f\x74\x6c\x69\x2d\x72"
        "\x65\x73\x70\x6f\x6e\x73\x65\x2d\x62\x6f\x64\x79\x03";
    HttpHeaderCaptureServer server;
    ASSERT_TRUE(http_header_capture_server_start(&server, 0, response, sizeof(response) - 1));

    char url[128];
    int url_size = snprintf(url, sizeof(url), "http://127.0.0.1:%d/document", server.port);
    ASSERT_GT(url_size, 0);
    ASSERT_LT(url_size, (int)sizeof(url));

    size_t content_size = 0;
    char* content = download_http_content(url, &content_size, NULL);

    char request[8192] = {};
    http_header_capture_server_finish(&server, request, sizeof(request));

    ASSERT_NE(nullptr, content);
    EXPECT_EQ(strlen("brotli-response-body"), content_size);
    EXPECT_STREQ("brotli-response-body", content);
    EXPECT_NE(nullptr, strstr(request, "Accept-Encoding:"));
    EXPECT_NE(nullptr, strstr(request, "br"));
    mem_free(content);
}

TEST_F(HttpInputTest, ErrorDocumentsRetainFinalResponseMetadataButResourcesReject) {
    const char response[] = "HTTP/1.1 503 Service Unavailable\r\n"
        "Content-Type: application/xhtml+xml\r\nContent-Language: fr\r\n"
        "Content-Length: 5\r\n\r\nerror";
    for (bool document : {false, true}) {
        HttpHeaderCaptureServer destination;
        ASSERT_TRUE(http_header_capture_server_start(&destination, 0, response, sizeof(response) - 1));
        char redirect[256];
        int length = snprintf(redirect, sizeof(redirect), "HTTP/1.1 302 Found\r\n"
            "Location: http://127.0.0.1:%d/final\r\nSet-Cookie: error=kept; Path=/\r\n"
            "Content-Length: 0\r\n\r\n", destination.port);
        ASSERT_GT(length, 0);
        ASSERT_LT((size_t)length, sizeof(redirect));
        HttpHeaderCaptureServer source;
        ASSERT_TRUE(http_header_capture_server_start(&source, 0, redirect, (size_t)length));
        char url[128], final_url[128];
        snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", source.port);
        snprintf(final_url, sizeof(final_url), "http://127.0.0.1:%d/final", destination.port);
        size_t size = 99;
        long status = 0;
        char* effective = NULL; char* language = NULL; char* type = NULL;
        char* body = document ? download_http_document_with_cookie_jar(url, &size, NULL,
            &effective, &language, &type, &status) : download_http_content(url, &size, NULL, &effective);
        char request[8192] = {};
        http_header_capture_server_finish(&source, request, sizeof(request));
        http_header_capture_server_finish(&destination, request, sizeof(request));
        if (document) {
            ASSERT_NE(body, nullptr);
            EXPECT_STREQ(body, "error");
            EXPECT_EQ(size, 5u);
            EXPECT_EQ(status, 503);
            EXPECT_STREQ(effective, final_url);
            EXPECT_STREQ(language, "fr");
            EXPECT_STREQ(type, "application/xhtml+xml");
            EXPECT_NE(strstr(request, "Cookie: error=kept"), nullptr);
        } else {
            EXPECT_EQ(body, nullptr);
            EXPECT_EQ(size, 0u);
            EXPECT_EQ(effective, nullptr);
        }
        mem_free(body); mem_free(effective); mem_free(language); mem_free(type);
    }
}

TEST_F(HttpInputTest, TruncatedErrorDocumentRemainsTransportFailure) {
    const char response[] = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\n"
        "Content-Length: 100\r\n\r\npartial";
    HttpHeaderCaptureServer server;
    ASSERT_TRUE(http_header_capture_server_start(&server, 0, response, sizeof(response) - 1));
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/document", server.port);
    size_t size = 99;
    long status = 99;
    char* body = download_http_document_with_cookie_jar(url, &size, NULL, NULL, NULL, NULL, &status);
    char request[8192] = {};
    http_header_capture_server_finish(&server, request, sizeof(request));
    EXPECT_EQ(body, nullptr);
    EXPECT_EQ(size, 0u);
    EXPECT_EQ(status, 0);
    mem_free(body);
}

TEST_F(HttpInputTest, TopLevelNavigationUsesPageLoadTimeout) {
    HttpHeaderCaptureServer server;
    // A 31-second first-byte delay exceeds the generic resource limit but is
    // valid within the document navigation's 60-second page-load budget.
    ASSERT_TRUE(http_header_capture_server_start(&server, 31));

    char url[128];
    int url_size = snprintf(url, sizeof(url), "http://127.0.0.1:%d/document", server.port);
    ASSERT_GT(url_size, 0);
    ASSERT_LT(url_size, (int)sizeof(url));

    size_t content_size = 0;
    char* content = download_http_content_with_cookie_jar(url, &content_size, NULL);

    char request[8192] = {};
    http_header_capture_server_finish(&server, request, sizeof(request));

    ASSERT_NE(nullptr, content);
    EXPECT_EQ(2u, content_size);
    EXPECT_STREQ("ok", content);
    mem_free(content);
}

// Test HTTP functionality
TEST_F(HttpInputTest, test_http_download) {
    // Test downloading a simple JSON file from GitHub API
    const char* test_url = "https://api.github.com/zen";

    printf("Testing HTTP download from: %s\n", test_url);

    // Create temp directory first
    system("mkdir -p ./temp/cache");

    // Test just the HTTP download function directly
    size_t content_size;
    char* content = download_http_content(test_url, &content_size, NULL);

    // Verify we got content
    ASSERT_NE(content, nullptr) << "HTTP download should not return null";
    ASSERT_GT(content_size, 0) << "Content size should be greater than 0";

    printf("Downloaded %zu bytes successfully\n", content_size);

    // Cleanup
    free(content);
}

TEST_F(HttpInputTest, test_http_cache) {
    // Test that caching works by downloading the same URL twice
    const char* test_url = "https://api.github.com/octocat";

    printf("Testing HTTP caching with: %s\n", test_url);

    // First download
    char* content1 = download_to_cache(test_url, "./temp/cache", NULL);
    ASSERT_NE(content1, nullptr) << "First download should succeed";

    // Second download (should use cache)
    char* content2 = download_to_cache(test_url, "./temp/cache", NULL);
    ASSERT_NE(content2, nullptr) << "Second download should succeed";

    // Note: UUID content will be different each time, so don't check for equality
    // This test just verifies that the caching mechanism doesn't crash

    printf("HTTP caching test completed successfully\n");

    // Cleanup
    free(content1);
    free(content2);
}

TEST_F(HttpInputTest, test_https_ssl) {
    // Test HTTPS with SSL verification
    const char* test_url = "https://api.github.com/zen";

    printf("Testing HTTPS with SSL verification: %s\n", test_url);

    size_t content_size;
    char* content = download_http_content(test_url, &content_size, NULL);

    ASSERT_NE(content, nullptr) << "HTTPS download should succeed";
    ASSERT_GT(content_size, 0) << "Content size should be greater than 0";

    printf("HTTPS SSL test completed successfully\n");
    printf("Downloaded %zu bytes: %.100s%s\n",
           content_size, content, content_size > 100 ? "..." : "");

    // Cleanup
    free(content);
}

TEST_F(HttpInputTest, test_http_error_handling) {
    // Test error handling with invalid URL
    const char* invalid_url = "https://api.github.com/this-definitely-does-not-exist-404";

    printf("Testing HTTP error handling with: %s\n", invalid_url);

    char* content = download_http_content(invalid_url, NULL, NULL);

    // Should return NULL for 404 error
    ASSERT_EQ(content, nullptr) << "404 URL should return null";

    printf("HTTP error handling test completed successfully\n");
}

TEST_F(HttpInputTest, DocumentCookiesKeepFinalPathAndHttpOnlyAttributes) {
    CookieJar* jar = cookie_jar_create(nullptr);
    ASSERT_NE(jar, nullptr);
    const char response[] = "HTTP/1.1 200 OK\r\n"
        "Set-Cookie: visible=allowed; Path=/\r\n"
        "Set-Cookie: private=secret; Path=/; HttpOnly\r\n"
        "Content-Length: 2\r\nConnection: close\r\n\r\nok";
    HttpHeaderCaptureServer server;
    ASSERT_TRUE(http_header_capture_server_start(&server, 0, response, sizeof(response) - 1));
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/document", server.port);
    size_t size = 0;
    char* body = download_http_document_with_cookie_jar(url, &size, jar, nullptr, nullptr);
    char request[8192] = {};
    http_header_capture_server_finish(&server, request, sizeof(request));
    EXPECT_STREQ(body, "ok");
    mem_free(body);
    char* visible = cookie_jar_build_document_cookie(jar, url);
    EXPECT_STREQ(visible, "visible=allowed");
    mem_free(visible);
    char* transport = cookie_jar_build_request_cookie(jar, url);
    ASSERT_NE(transport, nullptr);
    EXPECT_NE(strstr(transport, "visible=allowed"), nullptr);
    EXPECT_NE(strstr(transport, "private=secret"), nullptr);
    mem_free(transport);
    cookie_jar_destroy(jar);
}

TEST_F(HttpInputTest, CookiesShareAHostnameAcrossPorts) {
    CookieJar* jar = cookie_jar_create(nullptr);
    ASSERT_NE(jar, nullptr);
    cookie_jar_store(jar, "http://127.0.0.1:8080/source", "shared=allowed; Path=/");
    char* same_host = cookie_jar_build_document_cookie(jar, "http://127.0.0.1:8081/target");
    EXPECT_STREQ(same_host, "shared=allowed");
    mem_free(same_host);
    char* other_host = cookie_jar_build_document_cookie(jar, "http://example.test:8080/target");
    EXPECT_EQ(other_host, nullptr);
    mem_free(other_host);
    cookie_jar_destroy(jar);
}

TEST_F(HttpInputTest, FileDocumentCookiesRetainTheirEmptyHostname) {
    CookieJar* jar = cookie_jar_create(nullptr);
    ASSERT_NE(jar, nullptr);
    cookie_jar_store(jar, "file:///source.html", "theme=dark; Path=/");
    char* value = cookie_jar_build_document_cookie(jar, "file:///target.html");
    EXPECT_STREQ(value, "theme=dark");
    mem_free(value);
    char* remote = cookie_jar_build_document_cookie(jar, "https://example.test/target.html");
    EXPECT_EQ(remote, nullptr);
    mem_free(remote);
    cookie_jar_destroy(jar);
}

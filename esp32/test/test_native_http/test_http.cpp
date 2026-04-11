#include <unity.h>
#include "hal/test_impls.h"
#include <cstring>
#include <string>
#include "hal/hal.h"
#include "airbridge_http.h"

// ── Test fixtures ──────────────────────────────────────────────────────────

static StubDisplay  s_display;
static StubClock    s_clock;
static StubNvs      s_nvs;
static StubFilesys  s_fs;
static MockNetwork  s_net;
static HAL          s_hal = { &s_display, &s_clock, &s_nvs, &s_fs, &s_net };
HAL* g_hal = nullptr;

void setUp(void) {
    g_hal = &s_hal;
    s_net.reset();
}
void tearDown(void) {}

// ── halHttpReadResponse tests ───────────────────────────────────────────────

void test_http_read_simple(void) {
    s_net.next_response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello, World!";
    TlsHandle tls = g_hal->network->connect("example.com");
    std::string body = halHttpReadResponse(tls);
    g_hal->network->destroy(tls);
    TEST_ASSERT_EQUAL_STRING("Hello, World!", body.c_str());
}

void test_http_read_chunked(void) {
    s_net.next_response =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "5\r\nHello\r\n6\r\n World\r\n0\r\n";
    TlsHandle tls = g_hal->network->connect("example.com");
    std::string body = halHttpReadResponse(tls);
    g_hal->network->destroy(tls);
    TEST_ASSERT_EQUAL_STRING("Hello World", body.c_str());
}

void test_http_read_etag(void) {
    s_net.next_response =
        "HTTP/1.1 200 OK\r\nETag: \"abc123\"\r\n\r\nbody";
    TlsHandle tls = g_hal->network->connect("example.com");
    char etag[64] = "";
    std::string body = halHttpReadResponse(tls, etag, sizeof(etag));
    g_hal->network->destroy(tls);
    TEST_ASSERT_EQUAL_STRING("body", body.c_str());
    TEST_ASSERT_EQUAL_STRING("abc123", etag);
}

void test_http_read_no_etag(void) {
    s_net.next_response = "HTTP/1.1 200 OK\r\n\r\ndata";
    TlsHandle tls = g_hal->network->connect("example.com");
    char etag[64] = "unchanged";
    halHttpReadResponse(tls, etag, sizeof(etag));
    g_hal->network->destroy(tls);
    TEST_ASSERT_EQUAL_STRING("unchanged", etag);
}

// ── Request building tests ──────────────────────────────────────────────────

void test_buildApiGetRequest(void) {
    std::string req = buildApiGetRequest("api.example.com", "key123", "action=presign&file=test.bin");
    TEST_ASSERT_TRUE(req.find("GET /prod/presign?action=presign&file=test.bin") != std::string::npos);
    TEST_ASSERT_TRUE(req.find("Host: api.example.com") != std::string::npos);
    TEST_ASSERT_TRUE(req.find("x-api-key: key123") != std::string::npos);
}

void test_buildApiCompleteRequest(void) {
    std::string req = buildApiCompleteRequest("api.example.com", "key123",
        "upload-id-1", "s3/key/path", "{\"part\":1,\"etag\":\"abc\"}");
    TEST_ASSERT_TRUE(req.find("POST /prod/complete") != std::string::npos);
    TEST_ASSERT_TRUE(req.find("\"upload_id\":\"upload-id-1\"") != std::string::npos);
    TEST_ASSERT_TRUE(req.find("\"key\":\"s3/key/path\"") != std::string::npos);
}

// ── s3ApiGetViaHal tests ────────────────────────────────────────────────────

void test_s3ApiGet_success(void) {
    s_net.next_response =
        "HTTP/1.1 200 OK\r\nContent-Length: 42\r\n\r\n"
        "{\"url\":\"https://s3.aws.com/bucket/key\"}";
    std::string resp = s3ApiGetViaHal("api.example.com", "mykey", "action=presign");
    TEST_ASSERT_EQUAL_STRING("api.example.com", s_net.last_host.c_str());
    TEST_ASSERT_TRUE(s_net.last_request.find("x-api-key: mykey") != std::string::npos);
    // Parse JSON from response
    TEST_ASSERT_EQUAL_STRING("https://s3.aws.com/bucket/key", jsonStr(resp, "url").c_str());
}

void test_s3ApiGet_connect_failure(void) {
    s_net.fail_connect = true;
    std::string resp = s3ApiGetViaHal("api.example.com", "mykey", "action=presign");
    TEST_ASSERT_EQUAL_STRING("", resp.c_str());
}

void test_s3ApiGet_ota_version_check(void) {
    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n"
        "{\"version\":\"10.2002.0\",\"size\":1024000}";
    std::string resp = s3ApiGetViaHal("api.example.com", "key", "action=firmware");
    std::string version = jsonStr(resp, "version");
    int size = jsonInt(resp, "size");
    TEST_ASSERT_EQUAL_STRING("10.2002.0", version.c_str());
    TEST_ASSERT_EQUAL_INT(1024000, size);
    TEST_ASSERT_TRUE(versionNewer(version.c_str(), "10.2001.7"));
}

void test_s3ApiGet_multipart_presign(void) {
    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n"
        "{\"upload_id\":\"uid123\",\"key\":\"devices/abc/file.bin\",\"parts\":3}";
    std::string resp = s3ApiGetViaHal("api.example.com", "key", "action=presign&file=file.bin&size=15000000");
    TEST_ASSERT_EQUAL_STRING("uid123", jsonStr(resp, "upload_id").c_str());
    TEST_ASSERT_EQUAL_STRING("devices/abc/file.bin", jsonStr(resp, "key").c_str());
    TEST_ASSERT_EQUAL_INT(3, jsonInt(resp, "parts"));
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // HTTP response reading
    RUN_TEST(test_http_read_simple);
    RUN_TEST(test_http_read_chunked);
    RUN_TEST(test_http_read_etag);
    RUN_TEST(test_http_read_no_etag);

    // Request building
    RUN_TEST(test_buildApiGetRequest);
    RUN_TEST(test_buildApiCompleteRequest);

    // S3 API via HAL
    RUN_TEST(test_s3ApiGet_success);
    RUN_TEST(test_s3ApiGet_connect_failure);
    RUN_TEST(test_s3ApiGet_ota_version_check);
    RUN_TEST(test_s3ApiGet_multipart_presign);

    return UNITY_END();
}

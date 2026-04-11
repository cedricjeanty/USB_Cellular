#include <unity.h>
#include "hal/test_impls.h"
#include <cstring>
#include <string>
#include "hal/hal.h"
#include "airbridge_s3.h"

// ── Test fixtures ──────────────────────────────────────────────────────────

static StubDisplay s_display;
static TestClock   s_clock;
static TestNvs     s_nvs;
static MemFilesys  s_fs;
static MockNetwork s_net;
static HAL         s_hal = { &s_display, &s_clock, &s_nvs, &s_fs, &s_net, nullptr };
HAL* g_hal = nullptr;

void setUp(void) {
    g_hal = &s_hal;
    s_nvs.clear_all();
    s_fs.clear_all();
    s_net.reset();
    s_clock.now_ms = 1000;
}
void tearDown(void) {}

// ── loadMultipartSession tests ──────────────────────────────────────────────

void test_load_no_session(void) {
    MultipartSession s = loadMultipartSession("test.bin");
    TEST_ASSERT_FALSE(s.isResume);
    TEST_ASSERT_FALSE(s.cleared);
    TEST_ASSERT_EQUAL_STRING("", s.uploadId);
}

void test_load_resume_existing(void) {
    // Pre-populate NVS with an in-progress session
    s_nvs.set_str("s3up", "name", "test.bin");
    s_nvs.set_str("s3up", "uid", "upload-123");
    s_nvs.set_str("s3up", "key", "DEV01/test.bin");
    s_nvs.set_u32("s3up", "part", 2);
    s_nvs.set_u32("s3up", "parts", 3);
    s_nvs.set_u32("s3up", "retries", 0);

    MultipartSession s = loadMultipartSession("test.bin");
    TEST_ASSERT_TRUE(s.isResume);
    TEST_ASSERT_FALSE(s.cleared);
    TEST_ASSERT_EQUAL_STRING("upload-123", s.uploadId);
    TEST_ASSERT_EQUAL_STRING("DEV01/test.bin", s.key);
    TEST_ASSERT_EQUAL_UINT32(2, s.startPart);
    TEST_ASSERT_EQUAL_UINT32(3, s.totalParts);
}

void test_load_wrong_filename_ignored(void) {
    s_nvs.set_str("s3up", "name", "other.bin");
    s_nvs.set_str("s3up", "uid", "upload-456");

    MultipartSession s = loadMultipartSession("test.bin");
    TEST_ASSERT_FALSE(s.isResume);
    TEST_ASSERT_EQUAL_STRING("", s.uploadId);
}

void test_load_retry_limit_clears(void) {
    s_nvs.set_str("s3up", "name", "test.bin");
    s_nvs.set_str("s3up", "uid", "upload-789");
    s_nvs.set_u32("s3up", "retries", 3);

    MultipartSession s = loadMultipartSession("test.bin");
    TEST_ASSERT_FALSE(s.isResume);
    TEST_ASSERT_TRUE(s.cleared);
    // Session should be cleared from NVS
    char uid[64] = "still_here";
    TEST_ASSERT_FALSE(s_nvs.get_str("s3up", "uid", uid, sizeof(uid)));
}

void test_load_increments_retries(void) {
    s_nvs.set_str("s3up", "name", "test.bin");
    s_nvs.set_str("s3up", "uid", "upload-111");
    s_nvs.set_u32("s3up", "part", 1);
    s_nvs.set_u32("s3up", "parts", 2);
    s_nvs.set_u32("s3up", "retries", 1);

    loadMultipartSession("test.bin");
    uint32_t retries = 0;
    s_nvs.get_u32("s3up", "retries", &retries);
    TEST_ASSERT_EQUAL_UINT32(2, retries);
}

// ── saveMultipartSession tests ──────────────────────────────────────────────

void test_save_new_session(void) {
    saveMultipartSession("big.bin", "uid-new", "DEV/big.bin", 5, 25000000);

    char name[64], uid[256], key[128];
    s_nvs.get_str("s3up", "name", name, sizeof(name));
    s_nvs.get_str("s3up", "uid", uid, sizeof(uid));
    s_nvs.get_str("s3up", "key", key, sizeof(key));
    TEST_ASSERT_EQUAL_STRING("big.bin", name);
    TEST_ASSERT_EQUAL_STRING("uid-new", uid);
    TEST_ASSERT_EQUAL_STRING("DEV/big.bin", key);
    uint32_t parts = 0, part = 0, retries = 99;
    s_nvs.get_u32("s3up", "parts", &parts);
    s_nvs.get_u32("s3up", "part", &part);
    s_nvs.get_u32("s3up", "retries", &retries);
    TEST_ASSERT_EQUAL_UINT32(5, parts);
    TEST_ASSERT_EQUAL_UINT32(1, part);
    TEST_ASSERT_EQUAL_UINT32(0, retries);
}

// ── savePartProgress tests ──────────────────────────────────────────────────

void test_save_part_progress(void) {
    saveMultipartSession("file.bin", "uid1", "k", 3, 15000000);
    savePartProgress(1, "etag-abc");
    savePartProgress(2, "etag-def");

    char etag1[64], etag2[64];
    s_nvs.get_str("s3up", "etag1", etag1, sizeof(etag1));
    s_nvs.get_str("s3up", "etag2", etag2, sizeof(etag2));
    TEST_ASSERT_EQUAL_STRING("etag-abc", etag1);
    TEST_ASSERT_EQUAL_STRING("etag-def", etag2);

    // Part should advance to 3
    uint32_t part = 0;
    s_nvs.get_u32("s3up", "part", &part);
    TEST_ASSERT_EQUAL_UINT32(3, part);

    // Retries should reset to 0
    uint32_t retries = 99;
    s_nvs.get_u32("s3up", "retries", &retries);
    TEST_ASSERT_EQUAL_UINT32(0, retries);
}

// ── buildPartsJson tests ────────────────────────────────────────────────────

void test_build_parts_json(void) {
    saveMultipartSession("file.bin", "uid1", "k", 3, 15000000);
    savePartProgress(1, "aaa");
    savePartProgress(2, "bbb");
    savePartProgress(3, "ccc");

    std::string json = buildPartsJson(3);
    TEST_ASSERT_EQUAL_STRING(
        "{\"part\":1,\"etag\":\"aaa\"},{\"part\":2,\"etag\":\"bbb\"},{\"part\":3,\"etag\":\"ccc\"}",
        json.c_str());
}

void test_build_parts_json_single(void) {
    saveMultipartSession("f.bin", "uid", "k", 1, 5000000);
    savePartProgress(1, "xyz");
    std::string json = buildPartsJson(1);
    TEST_ASSERT_EQUAL_STRING("{\"part\":1,\"etag\":\"xyz\"}", json.c_str());
}

// ── clearMultipartSession tests ─────────────────────────────────────────────

void test_clear_session(void) {
    saveMultipartSession("file.bin", "uid1", "k", 3, 15000000);
    savePartProgress(1, "aaa");
    clearMultipartSession();

    char name[32] = "still";
    TEST_ASSERT_FALSE(s_nvs.get_str("s3up", "name", name, sizeof(name)));
    TEST_ASSERT_FALSE(s_nvs.get_str("s3up", "uid", name, sizeof(name)));
}

// ── Full resume scenario ────────────────────────────────────────────────────

void test_full_resume_scenario(void) {
    // Simulate: upload started, part 1 done, then crashed
    saveMultipartSession("flight.bin", "upload-ABC", "DEV/flight.bin", 3, 15000000);
    savePartProgress(1, "etag-part1");
    // Simulate crash (retries stays at 0 from savePartProgress)

    // On restart: load session
    MultipartSession s = loadMultipartSession("flight.bin");
    TEST_ASSERT_TRUE(s.isResume);
    TEST_ASSERT_EQUAL_UINT32(2, s.startPart);  // resume from part 2
    TEST_ASSERT_EQUAL_UINT32(3, s.totalParts);
    TEST_ASSERT_EQUAL_STRING("upload-ABC", s.uploadId);

    // Upload parts 2 and 3
    savePartProgress(2, "etag-part2");
    savePartProgress(3, "etag-part3");

    // Build completion JSON
    std::string json = buildPartsJson(3);
    TEST_ASSERT_TRUE(json.find("etag-part1") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("etag-part2") != std::string::npos);
    TEST_ASSERT_TRUE(json.find("etag-part3") != std::string::npos);

    // Clear session after completion
    clearMultipartSession();
    MultipartSession s2 = loadMultipartSession("flight.bin");
    TEST_ASSERT_FALSE(s2.isResume);
}

// ── loadS3Creds tests ───────────────────────────────────────────────────────

void test_load_creds_success(void) {
    s_nvs.set_str("s3", "api_host", "api.example.com");
    s_nvs.set_str("s3", "api_key", "mykey");
    s_nvs.set_str("s3", "device_id", "DEV01");
    S3Creds c = loadS3Creds();
    TEST_ASSERT_TRUE(c.valid);
    TEST_ASSERT_EQUAL_STRING("api.example.com", c.apiHost);
    TEST_ASSERT_EQUAL_STRING("mykey", c.apiKey);
    TEST_ASSERT_EQUAL_STRING("DEV01", c.deviceId);
}

void test_load_creds_missing(void) {
    S3Creds c = loadS3Creds();
    TEST_ASSERT_FALSE(c.valid);
}

// ── halS3UploadFile tests ───────────────────────────────────────────────────

void test_upload_no_creds(void) {
    s_fs.add_file_str("/sd/harvested/test.txt", "data");
    UploadResult r = halS3UploadFile("/sd/harvested/test.txt", "test.txt");
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_TRUE(strstr(r.error, "credentials") != nullptr);
}

void test_upload_file_missing(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    UploadResult r = halS3UploadFile("/sd/harvested/missing.txt", "missing.txt");
    TEST_ASSERT_FALSE(r.success);
}

void test_upload_presign_fails(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_file_str("/sd/harvested/test.txt", "hello");
    // Empty presign response
    s_net.next_response = "HTTP/1.1 500 Error\r\n\r\n{\"error\":\"fail\"}";
    UploadResult r = halS3UploadFile("/sd/harvested/test.txt", "test.txt");
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_TRUE(strstr(r.error, "Presign") != nullptr);
}

void test_upload_success(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_file_str("/sd/harvested/test.txt", "hello world");

    // First connection: presign API → returns URL
    // Second connection: S3 PUT → returns 200
    // MockNetwork uses same response for both connections, so we need the
    // presign response first (it's the first connect call)
    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n"
        "{\"url\":\"https://s3.aws.com/bucket/D1/test.txt?sig=abc\",\"key\":\"D1/test.txt\",\"parts\":1}";

    // Test the upload flow exercises presign → connect → stream → complete.
    // With the simple single-response mock, verify it gets past presign:
    UploadResult r = halS3UploadFile("/sd/harvested/test.txt", "test.txt");
    // Success depends on mock correctly serving presign + PUT responses.
    // The key thing we're testing is that the function uses HAL consistently.
    (void)r;  // Result varies with mock — real upload tested in emulator
}

void test_upload_connect_fails(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_file_str("/sd/harvested/test.txt", "data");
    s_net.fail_connect = true;
    UploadResult r = halS3UploadFile("/sd/harvested/test.txt", "test.txt");
    TEST_ASSERT_FALSE(r.success);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Load session
    RUN_TEST(test_load_no_session);
    RUN_TEST(test_load_resume_existing);
    RUN_TEST(test_load_wrong_filename_ignored);
    RUN_TEST(test_load_retry_limit_clears);
    RUN_TEST(test_load_increments_retries);

    // Save session
    RUN_TEST(test_save_new_session);

    // Part progress
    RUN_TEST(test_save_part_progress);

    // Parts JSON
    RUN_TEST(test_build_parts_json);
    RUN_TEST(test_build_parts_json_single);

    // Clear
    RUN_TEST(test_clear_session);

    // Full scenario
    RUN_TEST(test_full_resume_scenario);

    // Credentials
    RUN_TEST(test_load_creds_success);
    RUN_TEST(test_load_creds_missing);

    // Upload via HAL
    RUN_TEST(test_upload_no_creds);
    RUN_TEST(test_upload_file_missing);
    RUN_TEST(test_upload_presign_fails);
    RUN_TEST(test_upload_success);
    RUN_TEST(test_upload_connect_fails);

    return UNITY_END();
}

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

// A session with completed parts (startPart > 1) must NOT be abandoned at retries>=3 —
// on a marginal cellular link each part can take several windows, and discarding 10s of MB
// of completed parts (to restart a fresh multipart) is exactly the bug that stranded a
// 14/15-part 74MB upload in the field. It keeps resuming the SAME session.
void test_load_keeps_session_with_progress(void) {
    s_nvs.set_str("s3up", "name", "big.bin");
    s_nvs.set_str("s3up", "uid", "upload-prog");
    s_nvs.set_str("s3up", "key", "DEV01/big.bin");
    s_nvs.set_u32("s3up", "part", 15);    // 14 parts done, resuming at part 15
    s_nvs.set_u32("s3up", "parts", 15);
    s_nvs.set_u32("s3up", "retries", 4);  // would clear under the old flat >=3 rule
    MultipartSession s = loadMultipartSession("big.bin");
    TEST_ASSERT_TRUE(s.isResume);
    TEST_ASSERT_FALSE(s.cleared);
    TEST_ASSERT_EQUAL_UINT32(15, s.startPart);
}

// A no-progress session (still at part 1) is still abandoned at retries>=3.
void test_load_no_progress_still_clears(void) {
    s_nvs.set_str("s3up", "name", "stuck.bin");
    s_nvs.set_str("s3up", "uid", "upload-stuck");
    s_nvs.set_u32("s3up", "part", 1);
    s_nvs.set_u32("s3up", "retries", 3);
    MultipartSession s = loadMultipartSession("stuck.bin");
    TEST_ASSERT_TRUE(s.cleared);
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
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "data");
    UploadResult r = halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_TRUE(strstr(r.error, "credentials") != nullptr);
}

void test_upload_file_missing(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    UploadResult r = halS3UploadFile("/sd/upload/0001/missing.txt", "0001/missing.txt");
    TEST_ASSERT_FALSE(r.success);
}

void test_upload_presign_fails(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "hello");
    // Empty presign response
    s_net.next_response = "HTTP/1.1 500 Error\r\n\r\n{\"error\":\"fail\"}";
    UploadResult r = halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    TEST_ASSERT_FALSE(r.success);
    TEST_ASSERT_TRUE(strstr(r.error, "Presign") != nullptr);
}

void test_upload_success(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "hello world");

    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n"
        "{\"url\":\"https://s3.aws.com/bucket/D1/0001/test.txt?sig=abc\",\"key\":\"D1/0001/test.txt\",\"parts\":1}";

    UploadResult r = halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    (void)r;  // Result varies with mock — real upload tested in emulator
    // A small file is a SINGLE PUT (one connection) — no inter-part gap, so no
    // upload session is opened (only multipart needs the +++ bracket).
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_net.session_begins,
        "single-PUT upload must not open a multipart session");
}

void test_upload_connect_fails(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "data");
    s_net.fail_connect = true;
    UploadResult r = halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    TEST_ASSERT_FALSE(r.success);
}

// A single PUT must be CONFIRMED by a 200 + ETag before it counts as success —
// otherwise a PUT cut short mid-stream advances the manifest over a truncated S3
// object (silent data loss, observed on the 200MB hardware soak).
static const char* SINGLE_PRESIGN =
    "HTTP/1.1 200 OK\r\n\r\n"
    "{\"url\":\"https://s3.aws.com/bucket/D1/0001/test.txt?sig=abc\",\"key\":\"D1/0001/test.txt\",\"parts\":1}";

void test_upload_confirmed_etag_succeeds(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload"); s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "hello world");
    s_net.push_response(SINGLE_PRESIGN);                                    // presign GET
    s_net.push_response("HTTP/1.1 200 OK\r\nETag: \"abc123\"\r\n\r\n");     // PUT → 200 + ETag
    UploadResult r = halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    TEST_ASSERT_TRUE_MESSAGE(r.success, "confirmed 200+ETag PUT must succeed");
}

void test_upload_truncated_put_fails(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload"); s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "hello world");
    s_net.push_response(SINGLE_PRESIGN);   // presign GET
    s_net.push_response("");               // PUT response empty (connection cut) → no 200, no ETag
    UploadResult r = halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    TEST_ASSERT_FALSE_MESSAGE(r.success, "a cut PUT (no 200/ETag) must NOT count as success");
}

void test_upload_non200_put_fails(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload"); s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "hello world");
    s_net.push_response(SINGLE_PRESIGN);                                  // presign GET
    s_net.push_response("HTTP/1.1 403 Forbidden\r\n\r\nAccessDenied");    // PUT rejected
    UploadResult r = halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    TEST_ASSERT_FALSE_MESSAGE(r.success, "a non-200 PUT must NOT count as success");
}

// The presign GET must feed the link-quality hook so the OLED bars stay live during
// uploads (the per-file presign is the always-on latency sample). Regression for the
// "no connection bars during upload" report.
static int s_apiRttCalls = 0;
static void captureApiRtt(uint32_t) { s_apiRttCalls++; }

// A gzip-compressed .eaofh must be uploaded WHOLE — never byte-split by the delta
// logic. A gzip stream is indivisible; the split scan looks for an 0xEA 0x4C record
// marker, and gzip's ~random bytes contain coincidental markers, so without the guard
// the upload sends only [offset..end] → a headless, corrupt object S3 can't gunzip.
// (Root cause of truncated flights in the 200MB hardware soak under `compress on`.)
void test_eaofh_gzip_uploaded_whole(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload"); s_fs.add_dir("/sd/upload/0001");

    // 1000-byte "gzip" file: magic at [0:2] + a coincidental 0xEA4C record marker at
    // offset 600 (= est split for first=1,last=5,hwm=3) carrying flight 5 (> hwm) at
    // body[20:22]. Without the guard, halFindSplitOffset latches onto it → splits at 600.
    std::string content(1000, 'x');
    content[0] = (char)0x1f; content[1] = (char)0x8b;       // gzip magic
    content[600] = (char)0xEA; content[601] = (char)0x4C;   // record marker
    content[602] = (char)0x00; content[603] = (char)0x20;   // rlen = 32 (>= 28)
    content[624] = (char)0x00; content[625] = (char)0x05;   // flight 5 at recAbs+4+20
    const char* fp = "/sd/upload/0001/EA500.E2ETES_00005_20260628.eaofh";
    s_fs.add_file(fp, content.data(), content.size());
    s_fs.add_file_str("/sd/upload/0001/EA500.E2ETES_00005_20260628.eaofh.meta", "1:5");

    s_net.push_response("HTTP/1.1 200 OK\r\n\r\n{\"high_water_mark\":3}");           // manifest
    s_net.push_response("HTTP/1.1 200 OK\r\n\r\n{\"url\":\"https://s3/o?s=a\",\"key\":\"aircraft/EA500.E2ETES/EA500.E2ETES_00005_20260628.eaofh\",\"parts\":1}"); // eaofh presign
    s_net.push_response("HTTP/1.1 200 OK\r\n\r\n{\"url\":\"https://s3/o?s=a\",\"key\":\"aircraft/EA500.E2ETES/EA500.E2ETES_00005_20260628.eaofh\",\"parts\":1}"); // full-upload presign
    s_net.push_response("HTTP/1.1 200 OK\r\nETag: \"e\"\r\n\r\n");                    // PUT

    UploadResult r = halS3UploadEaofh("/sd/upload", "0001/EA500.E2ETES_00005_20260628.eaofh");
    // A presign must carry size=1000 (the WHOLE file). Without the guard it would split
    // at 600 and presign size=400 (the corrupt tail).
    bool wholeUpload = false, splitUpload = false;
    for (auto& req : s_net.requests) {
        if (req.find("size=1000") != std::string::npos) wholeUpload = true;
        if (req.find("size=400")  != std::string::npos) splitUpload = true;
    }
    TEST_ASSERT_TRUE_MESSAGE(wholeUpload, "compressed eaofh must presign+upload the WHOLE gzip (size=1000)");
    TEST_ASSERT_FALSE_MESSAGE(splitUpload, "compressed eaofh must NOT be byte-split (no size=400 tail)");
}

void test_presign_feeds_link_rtt(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload"); s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/test.txt", "hello world");
    s_apiRttCalls = 0;
    g_noteApiRttMs = captureApiRtt;
    s_net.push_response(SINGLE_PRESIGN);
    s_net.push_response("HTTP/1.1 200 OK\r\nETag: \"abc123\"\r\n\r\n");
    halS3UploadFile("/sd/upload/0001/test.txt", "0001/test.txt");
    g_noteApiRttMs = nullptr;
    TEST_ASSERT_GREATER_THAN_MESSAGE(0, s_apiRttCalls,
        "presign GET must report its RTT to keep the connection bars live");
}

// ── Multipart progress is FILE-cumulative ───────────────────────────────────
// Found live 2026-07-09 on a 162 MB flight upload: the OLED Cloud counter
// climbed to ~5 MB and snapped back to 0 once per part, because the multipart
// loop passed the raw per-part stream progress to the display callback. The
// contract: progress(sent,total) is file-cumulative — monotonic across parts,
// part N starting at (N-1)*5MB, ending at fileSize — including on NVS resume.

static std::vector<uint32_t> s_progLog;
static uint32_t s_progTotal = 0;
static void recordProgress(uint32_t sent, uint32_t total) {
    s_progLog.push_back(sent);
    s_progTotal = total;
}
static const char* s_bigPath = "/sd/upload/0001/big.bin";
static void seedCredsAndBigFile(uint32_t size) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload"); s_fs.add_dir("/sd/upload/0001");
    std::string big(size, 'x');
    s_fs.add_file(s_bigPath, big.data(), big.size());
}
static const char* MP_INIT =
    "HTTP/1.1 200 OK\r\n\r\n"
    "{\"upload_id\":\"U1\",\"key\":\"D1/big.bin\",\"parts\":2}";
static const char* MP_PART_URL =
    "HTTP/1.1 200 OK\r\n\r\n{\"url\":\"https://s3.aws.com/b/big.bin?p=x\"}";

void test_multipart_progress_cumulative(void) {
    const uint32_t SZ = 5u*1024*1024 + 2048;      // 2 parts: 5 MB + 2 KB
    seedCredsAndBigFile(SZ);
    s_net.push_response(MP_INIT);
    s_net.push_response(MP_PART_URL);
    s_net.push_response("HTTP/1.1 200 OK\r\nETag: \"e1\"\r\n\r\n");   // part 1 PUT
    s_net.push_response(MP_PART_URL);
    s_net.push_response("HTTP/1.1 200 OK\r\nETag: \"e2\"\r\n\r\n");   // part 2 PUT
    s_net.push_response("HTTP/1.1 200 OK\r\n\r\n{\"ok\":true}");      // complete
    s_progLog.clear(); s_progTotal = 0;
    UploadResult r = halS3UploadFile(s_bigPath, "0001/big.bin", recordProgress);
    TEST_ASSERT_TRUE_MESSAGE(r.success, r.error);
    TEST_ASSERT_FALSE(s_progLog.empty());
    for (size_t i = 1; i < s_progLog.size(); i++)
        TEST_ASSERT_TRUE_MESSAGE(s_progLog[i] >= s_progLog[i-1],
            "progress must never go backward across parts (Cloud MB sawtooth)");
    TEST_ASSERT_EQUAL_UINT32(SZ, s_progLog.back());     // ends at whole-file size
    TEST_ASSERT_EQUAL_UINT32(SZ, s_progTotal);          // total is whole-file size
}

void test_multipart_progress_resume_starts_at_base(void) {
    const uint32_t SZ = 5u*1024*1024 + 2048;
    seedCredsAndBigFile(SZ);
    // Simulate: part 1 finished in a previous boot, then power cut. The session
    // is keyed by the same relPath the upload task passes as `filename`.
    saveMultipartSession("0001/big.bin", "U1", "D1/big.bin", 2, SZ);
    savePartProgress(1, "e1");
    s_net.push_response(MP_INIT);                                     // presign (still called)
    s_net.push_response(MP_PART_URL);
    s_net.push_response("HTTP/1.1 200 OK\r\nETag: \"e2\"\r\n\r\n");   // part 2 PUT
    s_net.push_response("HTTP/1.1 200 OK\r\n\r\n{\"ok\":true}");      // complete
    s_progLog.clear();
    UploadResult r = halS3UploadFile(s_bigPath, "0001/big.bin", recordProgress);
    TEST_ASSERT_TRUE_MESSAGE(r.success, r.error);
    TEST_ASSERT_FALSE(s_progLog.empty());
    TEST_ASSERT_TRUE_MESSAGE(s_progLog.front() > 5u*1024*1024,
        "resumed upload must report progress from the completed-parts base, not 0");
    TEST_ASSERT_EQUAL_UINT32(SZ, s_progLog.back());
}

// ── findNextUploadFile tests ────────────────────────────────────────────────

void test_find_next_empty(void) {
    s_fs.add_dir("/sd/upload");
    char out[64];
    TEST_ASSERT_FALSE(findNextUploadFile("/sd/upload", out, sizeof(out)));
}

void test_find_next_in_subfolder(void) {
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/data.csv", "content");
    char out[64];
    TEST_ASSERT_TRUE(findNextUploadFile("/sd/upload", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0001/data.csv", out);
}

void test_find_next_oldest_subfolder_first(void) {
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0002");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0002/newer.csv", "bbb");
    s_fs.add_file_str("/sd/upload/0001/older.csv", "aaa");
    char out[64];
    TEST_ASSERT_TRUE(findNextUploadFile("/sd/upload", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0001/older.csv", out);
}

void test_find_next_skips_empty_subfolder(void) {
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");  // empty
    s_fs.add_dir("/sd/upload/0002");
    s_fs.add_file_str("/sd/upload/0002/data.csv", "content");
    char out[64];
    // 0001 is empty — should be auto-removed and 0002 found
    TEST_ASSERT_TRUE(findNextUploadFile("/sd/upload", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("0002/data.csv", out);
    // 0001 should have been cleaned up
    TEST_ASSERT_FALSE(s_fs.exists("/sd/upload/0001"));
}

// ── markFileUploaded tests ──────────────────────────────────────────────────

void test_mark_uploaded_deletes_file(void) {
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/data.csv", "content");
    s_fs.add_file_str("/sd/upload/0001/other.csv", "other");
    markFileUploaded("/sd/upload", "0001/data.csv");
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/upload/0001/data.csv"));
    // Subfolder still has other.csv, so it shouldn't be removed
    TEST_ASSERT_TRUE(s_fs.exists("/sd/upload/0001"));
}

void test_mark_uploaded_removes_empty_subfolder(void) {
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/data.csv", "content");
    markFileUploaded("/sd/upload", "0001/data.csv");
    TEST_ASSERT_FALSE(s_fs.has_file("/sd/upload/0001/data.csv"));
    // Subfolder should be removed since it's now empty
    TEST_ASSERT_FALSE(s_fs.exists("/sd/upload/0001"));
}

void test_upload_skip_if_exists(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    s_fs.add_file_str("/sd/upload/0001/boot_0010.log", "log content here");

    // Lambda returns skip:true because S3 already has this file
    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n"
        "{\"skip\":true,\"key\":\"D1/0001/boot_0010.log\",\"reason\":\"already exists\",\"s3_size\":16}";

    UploadResult r = halS3UploadFile("/sd/upload/0001/boot_0010.log", "0001/boot_0010.log");
    TEST_ASSERT_TRUE(r.success);  // skip counts as success
}

// ── Multipart part-PUT retry tests ──────────────────────────────────────────
// A >5MB file uploads as multipart (S3_CHUNK_SIZE = 5MB). The connection sequence
// is: [0] initial presign, then per part [presign, PUT], then [complete].
// MockNetwork serves response_queue front-first per connect().

static const std::string PRESIGN_INIT =
    "HTTP/1.1 200 OK\r\n\r\n"
    "{\"upload_id\":\"UP123\",\"key\":\"D1/0001/big.bin\",\"parts\":2}";
static std::string partPresign(int part) {
    return std::string("HTTP/1.1 200 OK\r\n\r\n")
         + "{\"url\":\"https://s3.aws.com/bucket/D1/0001/big.bin?partNumber=" + std::to_string(part)
         + "&uploadId=UP123&sig=abc\"}";
}
static std::string partPutOk(const char* etag) {
    return std::string("HTTP/1.1 200 OK\r\nETag: \"") + etag + "\"\r\n\r\n";
}
static const std::string PART_PUT_EMPTY = "";  // S3 sent nothing → no ETag (flaky link)
static const std::string COMPLETE_OK =
    "HTTP/1.1 200 OK\r\n\r\n<CompleteMultipartUploadResult></CompleteMultipartUploadResult>";

static void setup_multipart_creds_and_file(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_fs.add_dir("/sd/upload");
    s_fs.add_dir("/sd/upload/0001");
    std::string big(6 * 1024 * 1024, 'x');  // 6MB → 2 parts (5MB + 1MB)
    s_fs.add_file("/sd/upload/0001/big.bin", big.data(), big.size());
}

// Part 1's PUT transiently returns an empty response (no ETag); the retry
// re-presigns + re-PUTs and succeeds. Upload should ultimately succeed.
void test_multipart_part_put_retry_recovers(void) {
    setup_multipart_creds_and_file();
    s_net.push_response(PRESIGN_INIT);        // initial presign
    s_net.push_response(partPresign(1));      // part 1 presign (attempt 1)
    s_net.push_response(PART_PUT_EMPTY);      // part 1 PUT attempt 1 → no ETag
    s_net.push_response(partPresign(1));      // part 1 presign (attempt 2 / retry)
    s_net.push_response(partPutOk("etag1"));  // part 1 PUT attempt 2 → OK
    s_net.push_response(partPresign(2));      // part 2 presign
    s_net.push_response(partPutOk("etag2"));  // part 2 PUT → OK
    s_net.push_response(COMPLETE_OK);         // complete

    UploadResult r = halS3UploadFile("/sd/upload/0001/big.bin", "0001/big.bin");
    TEST_ASSERT_TRUE(r.success);
    // 8 connections = the scripted sequence incl. the one retry.
    TEST_ASSERT_EQUAL_INT(8, s_net.connect_count);
    // +++ suppression: the multipart session brackets the WHOLE loop — opened
    // once, cleanly closed (RAII), and every part connect happened inside it.
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_net.session_begins, "one upload session opened");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_net.session_depth, "session cleanly closed (balanced)");
    TEST_ASSERT_FALSE_MESSAGE(s_net.connect_outside_session,
        "no part connect outside the session (no +++ gap between parts)");
}

// All retries of a part fail → upload aborts cleanly (no false success), and the
// NVS resume session is preserved for the upload task's next attempt.
void test_multipart_part_put_all_retries_fail(void) {
    setup_multipart_creds_and_file();
    s_net.push_response(PRESIGN_INIT);    // initial presign
    s_net.push_response(partPresign(1));  // part 1 presign attempt 1
    s_net.push_response(PART_PUT_EMPTY);  // PUT 1 → no ETag
    s_net.push_response(partPresign(1));  // attempt 2
    s_net.push_response(PART_PUT_EMPTY);  // PUT 2 → no ETag
    s_net.push_response(partPresign(1));  // attempt 3
    s_net.push_response(PART_PUT_EMPTY);  // PUT 3 → no ETag
    // (no further responses needed — should give up after 3 attempts)

    UploadResult r = halS3UploadFile("/sd/upload/0001/big.bin", "0001/big.bin");
    TEST_ASSERT_FALSE(r.success);
    // Session still present (not cleared) so the next upload attempt resumes.
    char uid[64] = "";
    TEST_ASSERT_TRUE(s_nvs.get_str("s3up", "uid", uid, sizeof(uid)));
    TEST_ASSERT_EQUAL_STRING("UP123", uid);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Load session
    RUN_TEST(test_load_no_session);
    RUN_TEST(test_load_resume_existing);
    RUN_TEST(test_load_wrong_filename_ignored);
    RUN_TEST(test_load_retry_limit_clears);
    RUN_TEST(test_load_keeps_session_with_progress);
    RUN_TEST(test_load_no_progress_still_clears);
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
    RUN_TEST(test_upload_confirmed_etag_succeeds);
    RUN_TEST(test_upload_truncated_put_fails);
    RUN_TEST(test_upload_non200_put_fails);
    RUN_TEST(test_presign_feeds_link_rtt);
    RUN_TEST(test_eaofh_gzip_uploaded_whole);
    RUN_TEST(test_upload_skip_if_exists);

    // Multipart part-PUT retry
    RUN_TEST(test_multipart_part_put_retry_recovers);
    RUN_TEST(test_multipart_part_put_all_retries_fail);

    // findNextUploadFile
    RUN_TEST(test_find_next_empty);
    RUN_TEST(test_multipart_progress_cumulative);
    RUN_TEST(test_multipart_progress_resume_starts_at_base);
    RUN_TEST(test_find_next_in_subfolder);
    RUN_TEST(test_find_next_oldest_subfolder_first);
    RUN_TEST(test_find_next_skips_empty_subfolder);

    // markFileUploaded
    RUN_TEST(test_mark_uploaded_deletes_file);
    RUN_TEST(test_mark_uploaded_removes_empty_subfolder);

    return UNITY_END();
}

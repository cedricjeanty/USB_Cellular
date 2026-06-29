// Remote command channel — fetch + execute over the (mock) cellular HTTP path.
// Verifies the S3 delivery routes through the SAME shared executor as USB airbridge.cmd,
// the boot-only gating (format_sd), the once-stripping, and the ack POST.
#include <unity.h>
#include "hal/test_impls.h"
#include "hal/hal.h"
#include "airbridge_s3.h"
#include "airbridge_commands.h"
#include <cstring>
#include <string>

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
    g_compress = false;
    // S3 creds so loadS3Creds().valid is true
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "EMU_test");
}
void tearDown(void) {}

// ── halFetchCommands ─────────────────────────────────────────────────────────

void test_fetch_returns_command_and_hits_right_path(void) {
    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n{\"cmd\":\"compress on\",\"size\":11}";
    char buf[256] = "";
    bool got = halFetchCommands("EMU_test", buf, sizeof(buf));
    TEST_ASSERT_TRUE(got);
    TEST_ASSERT_EQUAL_STRING("compress on", buf);
    TEST_ASSERT_TRUE_MESSAGE(s_net.last_request.find("GET /prod/command?device=EMU_test") != std::string::npos,
                             "must GET /prod/command?device=...");
}

void test_fetch_no_command_returns_false(void) {
    s_net.next_response = "HTTP/1.1 404 Not Found\r\n\r\n{\"error\":\"no command\"}";
    char buf[64] = "x";
    TEST_ASSERT_FALSE(halFetchCommands("EMU_test", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);  // cleared on no-command
}

// ── runCommandTextBuffer (the SHARED executor — identical to USB) ─────────────

void test_run_executes_compress_and_wifi(void) {
    CmdRunResult r = runCommandTextBuffer("compress on\nwifi NET PASS", /*runtimeOnly=*/false,
                                          "/diag", "/logs", "/upload",
                                          nullptr, 0, /*allowSdOps=*/false);
    TEST_ASSERT_TRUE(r.ran);
    TEST_ASSERT_TRUE_MESSAGE(g_compress, "compress directive must set the shared g_compress");
    // wifi went to NVS (cliSetWifi)
    char ssid[64] = "";
    s_nvs.get_str("wifi", "ssid0", ssid, sizeof(ssid));
    TEST_ASSERT_EQUAL_STRING("NET", ssid);
}

void test_format_is_boot_only(void) {
    // At runtime (runtimeOnly=true) format_sd is skipped...
    CmdRunResult rt = runCommandTextBuffer("format_sd", true, "/d", "/l", "/u", nullptr, 0, false);
    TEST_ASSERT_FALSE_MESSAGE(rt.format, "format_sd is boot-only — skipped at runtimeOnly=true");
    // ...but the command channel runs runtimeOnly=false, so it fires.
    CmdRunResult bt = runCommandTextBuffer("format_sd", false, "/d", "/l", "/u", nullptr, 0, false);
    TEST_ASSERT_TRUE_MESSAGE(bt.format, "format_sd must fire at runtimeOnly=false (the remote path)");
}

void test_once_directive_is_stripped(void) {
    char out[256] = "";
    // `compress` persists; `reboot once` is consumed → rewrite keeps only compress.
    CmdRunResult r = runCommandTextBuffer("compress on\nreboot once", false,
                                          "/d", "/l", "/u", out, sizeof(out), false);
    TEST_ASSERT_TRUE(r.reboot);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, r.rewrite, "rewrite=1 (persistent lines remain)");
    TEST_ASSERT_TRUE_MESSAGE(strstr(out, "compress") != nullptr, "compress kept");
    TEST_ASSERT_TRUE_MESSAGE(strstr(out, "reboot") == nullptr, "reboot once stripped");
}

void test_dumplogs_skipped_on_remote_path(void) {
    // allowSdOps=false must not touch the filesystem (no SD-mutex block in the field).
    // With an empty MemFilesys this still must not error; ran=true, no copy attempted.
    CmdRunResult r = runCommandTextBuffer("dump_logs", false, "/diag", "/logs", "/upload",
                                          nullptr, 0, /*allowSdOps=*/false);
    TEST_ASSERT_TRUE(r.ran);
}

// ── halAckCommand ────────────────────────────────────────────────────────────

void test_ack_posts_result(void) {
    s_net.next_response = "HTTP/1.1 200 OK\r\n\r\n{\"ok\":true}";
    bool ok = halAckCommand("EMU_test", "{\"ran\":true,\"format\":false}");
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_TRUE_MESSAGE(s_net.last_request.find("POST /prod/command/ack?device=EMU_test") != std::string::npos,
                             "must POST /prod/command/ack?device=...");
    TEST_ASSERT_TRUE_MESSAGE(s_net.last_request.find("{\"ran\":true") != std::string::npos,
                             "ack body must carry the result json");
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fetch_returns_command_and_hits_right_path);
    RUN_TEST(test_fetch_no_command_returns_false);
    RUN_TEST(test_run_executes_compress_and_wifi);
    RUN_TEST(test_format_is_boot_only);
    RUN_TEST(test_once_directive_is_stripped);
    RUN_TEST(test_dumplogs_skipped_on_remote_path);
    RUN_TEST(test_ack_posts_result);
    return UNITY_END();
}

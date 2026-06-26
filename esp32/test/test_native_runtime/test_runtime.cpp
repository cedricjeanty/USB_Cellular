#include <unity.h>
#include "hal/test_impls.h"
#include <cstring>
#include <string>
#include "hal/hal.h"
#include "airbridge_runtime.h"

static StubDisplay s_display;
static TestClock   s_clock;
static TestNvs     s_nvs;
static StubFilesys s_fs;
static MockNetwork s_net;
static HAL         s_hal = { &s_display, &s_clock, &s_nvs, &s_fs, &s_net, nullptr };
HAL* g_hal = nullptr;

void setUp(void) {
    g_hal = &s_hal;
    s_nvs.clear_all();
    s_net.reset();
    s_clock.now_ms = 1000;
}
void tearDown(void) {}

// ── SpeedTracker tests ──────────────────────────────────────────────────────

void test_speed_first_call_zero(void) {
    SpeedTracker t = {};
    float spd = t.update(0.0f, 1000);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, spd);
}

void test_speed_simple(void) {
    SpeedTracker t = {};
    t.update(0.0f, 1000);
    float spd = t.update(1.0f, 2000);  // 1MB in 1s = 1024 KB/s
    TEST_ASSERT_FLOAT_WITHIN(10.0f, 1024.0f, spd);
}

void test_speed_no_progress(void) {
    SpeedTracker t = {};
    t.update(5.0f, 1000);
    float spd = t.update(5.0f, 2000);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, spd);
}

void test_speed_short_interval_returns_last(void) {
    SpeedTracker t = {};
    t.update(0.0f, 1000);
    t.update(1.0f, 2000);  // establishes speed
    float spd = t.update(1.1f, 2050);  // 50ms — returns last speed
    TEST_ASSERT_TRUE(spd > 0);
}

void test_speed_sliding_window(void) {
    SpeedTracker t = {};
    // Feed 10 samples over 10 seconds
    for (int i = 0; i <= 10; i++) {
        t.update(i * 1.0f, i * 1000);  // 1MB/s = 1024 KB/s
    }
    float spd = t.update(11.0f, 11000);
    TEST_ASSERT_FLOAT_WITHIN(50.0f, 1024.0f, spd);
}

// ── LogBuffer tests ─────────────────────────────────────────────────────────

void test_log_write_basic(void) {
    LogBuffer log;
    log.clear();
    log.write(5000, "Hello %s", "world");
    std::string s = log.contents();
    TEST_ASSERT_TRUE(s.find("[+5s]") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("Hello world") != std::string::npos);
}

void test_log_write_multiple(void) {
    LogBuffer log;
    log.clear();
    log.write(1000, "First");
    log.write(2000, "Second");
    std::string s = log.contents();
    TEST_ASSERT_TRUE(s.find("First") != std::string::npos);
    TEST_ASSERT_TRUE(s.find("Second") != std::string::npos);
}

void test_log_discard_oldest(void) {
    LogBuffer log;
    log.clear();
    // Fill the buffer with many entries
    for (int i = 0; i < 500; i++) {
        log.write(i * 1000, "Log entry number %d with some padding text here", i);
    }
    // Buffer should not exceed LOG_BUF_SIZE
    TEST_ASSERT_TRUE(log.len < LOG_BUF_SIZE);
    TEST_ASSERT_TRUE(log.len > 0);
    // Oldest entries should be discarded, newest should remain
    std::string s = log.contents();
    TEST_ASSERT_TRUE(s.find("499") != std::string::npos);  // newest
    TEST_ASSERT_TRUE(s.find("entry number 0 ") == std::string::npos);  // oldest gone
}

void test_log_clear(void) {
    LogBuffer log;
    log.clear();
    log.write(0, "stuff");
    TEST_ASSERT_TRUE(log.len > 0);
    log.clear();
    TEST_ASSERT_EQUAL_INT(0, log.len);
}

// ── DeviceStatus formatting tests ───────────────────────────────────────────

void test_status_format_basic(void) {
    DeviceStatus s = {};
    s.filesQueued = 5;
    s.filesUploaded = 10;
    s.mbQueued = 25.5f;
    s.mbUploaded = 100.0f;
    s.pppConnected = true;
    s.modemRssi = 22;
    strlcpy(s.modemOp, "T-Mobile", sizeof(s.modemOp));
    strlcpy(s.fwVersion, "10.2001.7", sizeof(s.fwVersion));
    s.mscOnly = true;
    strlcpy(s.apiHost, "api.example.com", sizeof(s.apiHost));
    strlcpy(s.deviceId, "DEV01", sizeof(s.deviceId));

    std::string out = formatStatus(s);
    TEST_ASSERT_TRUE(out.find("files_q=5") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("files_up=10") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("net=cellular") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("T-Mobile") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("10.2001.7") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("MSC-only") != std::string::npos);
    TEST_ASSERT_TRUE(out.find("DEV01") != std::string::npos);
}

void test_status_no_network(void) {
    DeviceStatus s = {};
    strlcpy(s.fwVersion, "1.0.0", sizeof(s.fwVersion));
    std::string out = formatStatus(s);
    TEST_ASSERT_TRUE(out.find("net=none") != std::string::npos);
}

void test_status_wifi(void) {
    DeviceStatus s = {};
    s.wifiConnected = true;
    strlcpy(s.fwVersion, "1.0.0", sizeof(s.fwVersion));
    std::string out = formatStatus(s);
    TEST_ASSERT_TRUE(out.find("net=wifi") != std::string::npos);
}

void test_status_upload_speed_shown(void) {
    DeviceStatus s = {};
    s.lastUploadKBps = 450.0f;
    strlcpy(s.fwVersion, "1.0.0", sizeof(s.fwVersion));
    std::string out = formatStatus(s);
    TEST_ASSERT_TRUE(out.find("last_upload=450") != std::string::npos);
}

void test_status_upload_speed_hidden(void) {
    DeviceStatus s = {};
    s.lastUploadKBps = 0;
    strlcpy(s.fwVersion, "1.0.0", sizeof(s.fwVersion));
    std::string out = formatStatus(s);
    TEST_ASSERT_TRUE(out.find("last_upload") == std::string::npos);
}

// ── OTA check tests ─────────────────────────────────────────────────────────

void test_ota_no_creds(void) {
    OtaCheckResult r = halOtaCheck("10.2001.7");
    TEST_ASSERT_EQUAL_INT(0, r.status);
}

void test_ota_up_to_date(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n{\"version\":\"10.2001.7\",\"size\":800000}";
    OtaCheckResult r = halOtaCheck("10.2001.7");
    TEST_ASSERT_EQUAL_INT(0, r.status);  // same version = up to date
}

void test_ota_update_available(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    // First connect: version check returns newer version
    // Second connect: download URL
    // Mock returns same response for both, so the URL will be parsed from the JSON
    s_net.next_response =
        "HTTP/1.1 200 OK\r\n\r\n"
        "{\"version\":\"10.2002.0\",\"size\":900000,\"url\":\"https://s3.aws.com/fw.bin\"}";
    OtaCheckResult r = halOtaCheck("10.2001.7");
    TEST_ASSERT_EQUAL_INT(1, r.status);
    TEST_ASSERT_EQUAL_STRING("10.2002.0", r.newVersion);
    TEST_ASSERT_EQUAL_UINT32(900000, r.size);
}

void test_ota_connect_fails(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_net.fail_connect = true;
    OtaCheckResult r = halOtaCheck("10.2001.7");
    TEST_ASSERT_EQUAL_INT(-1, r.status);
}

void test_ota_no_firmware(void) {
    s_nvs.set_str("s3", "api_host", "api.ex.com");
    s_nvs.set_str("s3", "api_key", "key");
    s_nvs.set_str("s3", "device_id", "D1");
    s_net.next_response = "HTTP/1.1 404 Not Found\r\n\r\n{\"error\":\"no firmware\"}";
    OtaCheckResult r = halOtaCheck("10.2001.7");
    TEST_ASSERT_EQUAL_INT(0, r.status);  // no firmware = don't retry
}

// ── No-progress watchdog ────────────────────────────────────────────────────

void test_watchdog_not_started(void) {
    // Heartbeat 0 = main loop hasn't run yet → never reboot.
    TEST_ASSERT_FALSE(watchdogShouldReboot(0, 1000000, 300000));
}

void test_watchdog_fresh_heartbeat(void) {
    // Heartbeat recent → no reboot.
    TEST_ASSERT_FALSE(watchdogShouldReboot(1000000, 1000000, 300000));
    TEST_ASSERT_FALSE(watchdogShouldReboot(1000000, 1000000 + 299999, 300000));
}

void test_watchdog_stalled(void) {
    // Heartbeat older than the stall threshold → reboot.
    TEST_ASSERT_TRUE(watchdogShouldReboot(1000000, 1000000 + 300001, 300000));
    // The 9-hour field freeze: heartbeat frozen, hours elapse → definitely reboot.
    TEST_ASSERT_TRUE(watchdogShouldReboot(500000, 500000 + 9u*3600u*1000u, 300000));
}

void test_watchdog_millis_wraparound(void) {
    // now wrapped past 0; small real elapsed must NOT falsely trip.
    uint32_t hb = 0xFFFFFF00u;          // near uint32 max
    uint32_t now = 0x00000100u;         // wrapped; real elapsed = 0x200 = 512ms
    TEST_ASSERT_FALSE(watchdogShouldReboot(hb, now, 300000));
    // ...but a genuine long stall across the wrap still trips.
    TEST_ASSERT_TRUE(watchdogShouldReboot(hb, hb + 400000u, 300000));
}

// ── SD recovery escalation ──────────────────────────────────────────────────

void test_sd_recovery_healthy(void) {
    // Below both thresholds → keep probing, do nothing.
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_NONE, sdRecoveryAction(0, 5, 20));
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_NONE, sdRecoveryAction(4, 5, 20));
}

void test_sd_recovery_degrade_band(void) {
    // At/above degrade but below reformat → degrade (non-destructive).
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_DEGRADE, sdRecoveryAction(5, 5, 20));
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_DEGRADE, sdRecoveryAction(19, 5, 20));
}

void test_sd_recovery_reformat(void) {
    // At/above reformat → destructive recovery.
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_REFORMAT, sdRecoveryAction(20, 5, 20));
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_REFORMAT, sdRecoveryAction(1000, 5, 20));
}

void test_sd_recovery_disabled_tiers(void) {
    // A zero threshold disables that tier entirely.
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_NONE, sdRecoveryAction(100, 0, 0));
    // reformat disabled, degrade enabled → never escalates past degrade.
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_DEGRADE, sdRecoveryAction(100, 5, 0));
    // degrade disabled, reformat enabled → jumps straight to reformat at threshold.
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_NONE,     sdRecoveryAction(10, 0, 20));
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_REFORMAT, sdRecoveryAction(20, 0, 20));
}

void test_sd_health_update_resets_on_ok(void) {
    SdHealthState st;
    sdHealthUpdate(st, false, 2, 5);
    sdHealthUpdate(st, false, 2, 5);
    TEST_ASSERT_EQUAL_UINT(2, st.consecutiveFailures);
    SdRecoveryAction a = sdHealthUpdate(st, true, 2, 5);  // ok → reset
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_NONE, a);
    TEST_ASSERT_EQUAL_UINT(0, st.consecutiveFailures);
}

void test_sd_health_update_escalates(void) {
    SdHealthState st;
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_NONE,     sdHealthUpdate(st, false, 2, 5)); // 1
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_DEGRADE,  sdHealthUpdate(st, false, 2, 5)); // 2
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_DEGRADE,  sdHealthUpdate(st, false, 2, 5)); // 3
    sdHealthUpdate(st, false, 2, 5);                                              // 4
    TEST_ASSERT_EQUAL_INT(SD_RECOVERY_REFORMAT, sdHealthUpdate(st, false, 2, 5)); // 5
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // SpeedTracker
    RUN_TEST(test_speed_first_call_zero);
    RUN_TEST(test_speed_simple);
    RUN_TEST(test_speed_no_progress);
    RUN_TEST(test_speed_short_interval_returns_last);
    RUN_TEST(test_speed_sliding_window);

    // LogBuffer
    RUN_TEST(test_log_write_basic);
    RUN_TEST(test_log_write_multiple);
    RUN_TEST(test_log_discard_oldest);
    RUN_TEST(test_log_clear);

    // DeviceStatus
    RUN_TEST(test_status_format_basic);
    RUN_TEST(test_status_no_network);
    RUN_TEST(test_status_wifi);
    RUN_TEST(test_status_upload_speed_shown);
    RUN_TEST(test_status_upload_speed_hidden);

    // OTA check
    RUN_TEST(test_ota_no_creds);
    RUN_TEST(test_ota_up_to_date);
    RUN_TEST(test_ota_update_available);
    RUN_TEST(test_ota_connect_fails);
    RUN_TEST(test_ota_no_firmware);

    RUN_TEST(test_watchdog_not_started);
    RUN_TEST(test_watchdog_fresh_heartbeat);
    RUN_TEST(test_watchdog_stalled);
    RUN_TEST(test_watchdog_millis_wraparound);

    RUN_TEST(test_sd_recovery_healthy);
    RUN_TEST(test_sd_recovery_degrade_band);
    RUN_TEST(test_sd_recovery_reformat);
    RUN_TEST(test_sd_recovery_disabled_tiers);
    RUN_TEST(test_sd_health_update_resets_on_ok);
    RUN_TEST(test_sd_health_update_escalates);

    return UNITY_END();
}

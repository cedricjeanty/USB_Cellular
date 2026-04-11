#include <unity.h>
#include "hal/test_impls.h"
#include <cstring>
#include <string>
#include "hal/hal.h"
#include "airbridge_cli.h"

// ── Test fixtures ──────────────────────────────────────────────────────────

static StubDisplay s_display;
static StubClock   s_clock;
static TestNvs     s_nvs;
static HAL         s_hal = { &s_display, &s_clock, &s_nvs, nullptr, nullptr, nullptr };
HAL* g_hal = nullptr;

void setUp(void) { g_hal = &s_hal; s_nvs.clear_all(); }
void tearDown(void) {}

// ── SETWIFI tests ───────────────────────────────────────────────────────────

void test_setwifi_with_password(void) {
    CliResult r = cliSetWifi("MyNetwork secret123");
    TEST_ASSERT_TRUE(r.handled);
    TEST_ASSERT_TRUE(strstr(r.output, "MyNetwork") != nullptr);
    // Verify it was saved via NVS
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("MyNetwork", nets[0].ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", nets[0].pass);
}

void test_setwifi_no_password(void) {
    CliResult r = cliSetWifi("OpenNet");
    TEST_ASSERT_TRUE(r.handled);
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("OpenNet", nets[0].ssid);
    TEST_ASSERT_EQUAL_STRING("", nets[0].pass);
}

void test_setwifi_empty(void) {
    CliResult r = cliSetWifi("");
    TEST_ASSERT_TRUE(strstr(r.output, "SETWIFI") != nullptr);
}

void test_setwifi_password_with_spaces(void) {
    // "MyNet my pass word" → ssid="MyNet", pass="my pass word"
    CliResult r = cliSetWifi("MyNet my pass word");
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("MyNet", nets[0].ssid);
    TEST_ASSERT_EQUAL_STRING("my pass word", nets[0].pass);
}

// ── SETS3 tests ─────────────────────────────────────────────────────────────

void test_sets3_success(void) {
    CliResult r = cliSetS3("api.example.com myApiKey123");
    TEST_ASSERT_TRUE(r.handled);
    TEST_ASSERT_TRUE(strstr(r.output, "api.example.com") != nullptr);
    // Verify NVS
    char host[128] = "", key[64] = "";
    s_nvs.get_str("s3", "api_host", host, sizeof(host));
    s_nvs.get_str("s3", "api_key", key, sizeof(key));
    TEST_ASSERT_EQUAL_STRING("api.example.com", host);
    TEST_ASSERT_EQUAL_STRING("myApiKey123", key);
}

void test_sets3_missing_key(void) {
    CliResult r = cliSetS3("api.example.com");
    TEST_ASSERT_TRUE(strstr(r.output, "SETS3") != nullptr);
}

void test_sets3_empty(void) {
    CliResult r = cliSetS3("");
    TEST_ASSERT_TRUE(strstr(r.output, "SETS3") != nullptr);
}

// ── SETMODE tests ───────────────────────────────────────────────────────────

void test_setmode_cdc(void) {
    SetModeResult r = cliSetMode("CDC", true);
    TEST_ASSERT_EQUAL_INT(0, r.mode);
    TEST_ASSERT_TRUE(strstr(r.output, "CDC+MSC") != nullptr);
    // Verify NVS
    uint8_t val = 99;
    s_nvs.get_u8("usb", "msc_only", &val);
    TEST_ASSERT_EQUAL_UINT8(0, val);
}

void test_setmode_msc(void) {
    SetModeResult r = cliSetMode("MSC", false);
    TEST_ASSERT_EQUAL_INT(1, r.mode);
    TEST_ASSERT_TRUE(strstr(r.output, "MSC-only") != nullptr);
    uint8_t val = 99;
    s_nvs.get_u8("usb", "msc_only", &val);
    TEST_ASSERT_EQUAL_UINT8(1, val);
}

void test_setmode_query(void) {
    SetModeResult r = cliSetMode("", true);
    TEST_ASSERT_EQUAL_INT(-2, r.mode);
    TEST_ASSERT_TRUE(strstr(r.output, "current=MSC") != nullptr);
}

void test_setmode_query_cdc(void) {
    SetModeResult r = cliSetMode("", false);
    TEST_ASSERT_EQUAL_INT(-2, r.mode);
    TEST_ASSERT_TRUE(strstr(r.output, "current=CDC") != nullptr);
}

void test_setmode_invalid(void) {
    SetModeResult r = cliSetMode("INVALID", false);
    TEST_ASSERT_EQUAL_INT(-1, r.mode);
    TEST_ASSERT_TRUE(strstr(r.output, "unknown") != nullptr);
}

void test_setmode_case_insensitive(void) {
    SetModeResult r = cliSetMode("cdc", true);
    TEST_ASSERT_EQUAL_INT(0, r.mode);
    r = cliSetMode("msc", false);
    TEST_ASSERT_EQUAL_INT(1, r.mode);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // SETWIFI
    RUN_TEST(test_setwifi_with_password);
    RUN_TEST(test_setwifi_no_password);
    RUN_TEST(test_setwifi_empty);
    RUN_TEST(test_setwifi_password_with_spaces);

    // SETS3
    RUN_TEST(test_sets3_success);
    RUN_TEST(test_sets3_missing_key);
    RUN_TEST(test_sets3_empty);

    // SETMODE
    RUN_TEST(test_setmode_cdc);
    RUN_TEST(test_setmode_msc);
    RUN_TEST(test_setmode_query);
    RUN_TEST(test_setmode_query_cdc);
    RUN_TEST(test_setmode_invalid);
    RUN_TEST(test_setmode_case_insensitive);

    return UNITY_END();
}

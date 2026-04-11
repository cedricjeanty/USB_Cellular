#include <unity.h>
#include "hal/test_impls.h"
#include <cstring>
#include <string>
#include "hal/hal.h"
#include "airbridge_wifi_creds.h"

// ── Test fixtures ──────────────────────────────────────────────────────────

static StubDisplay s_display;
static StubClock   s_clock;
static TestNvs     s_nvs;
static HAL         s_hal = { &s_display, &s_clock, &s_nvs };
HAL* g_hal = nullptr;

void setUp(void) {
    g_hal = &s_hal;
    s_nvs.clear_all();
}
void tearDown(void) {}

// ── INvs basic tests ────────────────────────────────────────────────────────

void test_nvs_str_roundtrip(void) {
    g_hal->nvs->set_str("test", "key1", "hello");
    char buf[32] = "";
    TEST_ASSERT_TRUE(g_hal->nvs->get_str("test", "key1", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("hello", buf);
}

void test_nvs_str_missing(void) {
    char buf[32] = "old";
    TEST_ASSERT_FALSE(g_hal->nvs->get_str("test", "nokey", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_nvs_i32_roundtrip(void) {
    g_hal->nvs->set_i32("test", "count", 42);
    int32_t v = 0;
    TEST_ASSERT_TRUE(g_hal->nvs->get_i32("test", "count", &v));
    TEST_ASSERT_EQUAL_INT32(42, v);
}

void test_nvs_u32_roundtrip(void) {
    g_hal->nvs->set_u32("dbg", "boots", 100);
    uint32_t v = 0;
    TEST_ASSERT_TRUE(g_hal->nvs->get_u32("dbg", "boots", &v));
    TEST_ASSERT_EQUAL_UINT32(100, v);
}

void test_nvs_erase(void) {
    g_hal->nvs->set_str("test", "key", "val");
    g_hal->nvs->erase_key("test", "key");
    char buf[16] = "";
    TEST_ASSERT_FALSE(g_hal->nvs->get_str("test", "key", buf, sizeof(buf)));
}

void test_nvs_namespaces_isolated(void) {
    g_hal->nvs->set_str("ns1", "key", "a");
    g_hal->nvs->set_str("ns2", "key", "b");
    char buf[16];
    g_hal->nvs->get_str("ns1", "key", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("a", buf);
    g_hal->nvs->get_str("ns2", "key", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("b", buf);
}

// ── WiFi credential MRU tests ───────────────────────────────────────────────

void test_saveNetwork_single(void) {
    saveNetwork("MyWiFi", "pass123");
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("MyWiFi", nets[0].ssid);
    TEST_ASSERT_EQUAL_STRING("pass123", nets[0].pass);
}

void test_saveNetwork_mru_order(void) {
    saveNetwork("Net1", "p1");
    saveNetwork("Net2", "p2");
    saveNetwork("Net3", "p3");
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING("Net3", nets[0].ssid);
    TEST_ASSERT_EQUAL_STRING("Net2", nets[1].ssid);
    TEST_ASSERT_EQUAL_STRING("Net1", nets[2].ssid);
}

void test_saveNetwork_dedup(void) {
    saveNetwork("Net1", "p1");
    saveNetwork("Net2", "p2");
    saveNetwork("Net1", "p1_new");
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("Net1", nets[0].ssid);
    TEST_ASSERT_EQUAL_STRING("p1_new", nets[0].pass);
    TEST_ASSERT_EQUAL_STRING("Net2", nets[1].ssid);
}

void test_saveNetwork_max_cap(void) {
    for (int i = 0; i < 7; i++) {
        char ssid[8], pass[8];
        snprintf(ssid, sizeof(ssid), "Net%d", i);
        snprintf(pass, sizeof(pass), "p%d", i);
        saveNetwork(ssid, pass);
    }
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(MAX_KNOWN_NETS, n);
    // Most recent should be first
    TEST_ASSERT_EQUAL_STRING("Net6", nets[0].ssid);
}

void test_loadKnownNets_empty(void) {
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    TEST_ASSERT_EQUAL_INT(0, n);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // INvs basics
    RUN_TEST(test_nvs_str_roundtrip);
    RUN_TEST(test_nvs_str_missing);
    RUN_TEST(test_nvs_i32_roundtrip);
    RUN_TEST(test_nvs_u32_roundtrip);
    RUN_TEST(test_nvs_erase);
    RUN_TEST(test_nvs_namespaces_isolated);

    // WiFi credential MRU
    RUN_TEST(test_saveNetwork_single);
    RUN_TEST(test_saveNetwork_mru_order);
    RUN_TEST(test_saveNetwork_dedup);
    RUN_TEST(test_saveNetwork_max_cap);
    RUN_TEST(test_loadKnownNets_empty);

    return UNITY_END();
}

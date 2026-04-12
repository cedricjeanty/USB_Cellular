// Tests for the extracted modem init sequence running against SimModem via PTY.
// This exercises the same AT command flow that runs on the real device.

#include <unity.h>
#include <cstring>
#include <unistd.h>
#include "hal/hal.h"
#include "hal/test_impls.h"
#include "hal/uart_pty.h"
#include "sim_modem.h"
#include "airbridge_modem.h"

// ── mdm_* function definitions (normally in main.cpp) ───────────────────────

static PtyUart* s_uart_ptr = nullptr;

int mdm_write(const void* data, size_t len) {
    return s_uart_ptr->write(data, len);
}
int mdm_read(void* buf, size_t len, uint32_t timeout_ms) {
    return s_uart_ptr->read(buf, len, timeout_ms);
}
void mdm_flush() { s_uart_ptr->flush(); }
void mdm_set_baudrate(uint32_t baud) { s_uart_ptr->set_baudrate(baud); }

int modem_at_cmd(const char* cmd, char* resp, int resp_size, int timeout_ms) {
    mdm_write(cmd, strlen(cmd));
    mdm_write("\r", 1);
    int total = 0;
    uint32_t start = g_hal->clock->millis();
    while ((g_hal->clock->millis() - start) < (uint32_t)timeout_ms && total < resp_size - 1) {
        uint8_t buf[128];
        int len = mdm_read(buf, sizeof(buf), 100);
        if (len > 0) {
            int copy = (len < resp_size - 1 - total) ? len : resp_size - 1 - total;
            memcpy(resp + total, buf, copy);
            total += copy;
            resp[total] = '\0';
            if (strstr(resp, "OK") || strstr(resp, "ERROR") || strstr(resp, "CONNECT"))
                break;
        }
    }
    resp[total] = '\0';
    return total;
}

// ── Test fixtures ───────────────────────────────────────────────────────────

static SimModem*   s_modem = nullptr;
static PtyUart     s_uart;
// Real-time clock — SimModem uses wall-clock for +++ escape detection
class RealClock : public IClock {
public:
    uint32_t millis() override {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    void delay_ms(uint32_t ms) override { usleep(ms * 1000); }
};

static StubDisplay s_display;
static RealClock   s_clock;
static HAL         s_hal = { &s_display, &s_clock, nullptr, nullptr, nullptr, &s_uart };
HAL* g_hal = nullptr;

void setUp(void) {
    g_hal = &s_hal;
    if (s_modem) { s_modem->stop(); delete s_modem; }
    s_modem = new SimModem();
    s_modem->operatorName = "TestNet";
    s_modem->rssi = 18;
    s_modem->regStat = 1;
    s_modem->echoEnabled = false;
    s_modem->start();
    s_uart.fd = s_modem->slave_fd;
    s_uart_ptr = &s_uart;
    usleep(50000);
}

void tearDown(void) {
    if (s_modem) { s_modem->stop(); delete s_modem; s_modem = nullptr; }
}

// ── Tests ───────────────────────────────────────────────────────────────────

void test_at_sync(void) {
    bool synced = modemAtSync();
    TEST_ASSERT_TRUE(synced);
}

void test_full_init(void) {
    // Must sync first
    bool synced = modemAtSync();
    TEST_ASSERT_TRUE(synced);

    ModemInitResult r = modemRunInit();
    TEST_ASSERT_TRUE(r.synced);
    TEST_ASSERT_TRUE(r.registered);
    TEST_ASSERT_TRUE(r.connected);
    TEST_ASSERT_EQUAL_INT(18, r.rssi);
    TEST_ASSERT_EQUAL_STRING("TestNet", r.operatorName);
    TEST_ASSERT_TRUE(r.epoch > 0);  // SimModem returns real UTC time
}

void test_init_no_registration(void) {
    s_modem->regStat = 2;  // searching, never registers

    bool synced = modemAtSync();
    TEST_ASSERT_TRUE(synced);

    ModemInitResult r = modemRunInit();
    TEST_ASSERT_TRUE(r.synced);
    TEST_ASSERT_FALSE(r.registered);
    // Still dials PPP even without registration
    TEST_ASSERT_TRUE(r.connected);
}

void test_init_roaming(void) {
    s_modem->regStat = 5;  // roaming

    modemAtSync();
    ModemInitResult r = modemRunInit();
    TEST_ASSERT_TRUE(r.registered);
    TEST_ASSERT_TRUE(r.connected);
}

void test_init_weak_signal(void) {
    s_modem->rssi = 5;

    modemAtSync();
    ModemInitResult r = modemRunInit();
    TEST_ASSERT_EQUAL_INT(5, r.rssi);
    TEST_ASSERT_TRUE(r.connected);
}

void test_init_different_operator(void) {
    s_modem->operatorName = "AT&T";

    modemAtSync();
    ModemInitResult r = modemRunInit();
    TEST_ASSERT_EQUAL_STRING("AT&T", r.operatorName);
}

void test_init_time_sync(void) {
    modemAtSync();
    ModemInitResult r = modemRunInit();
    // SimModem returns current UTC time via +CCLK
    // Epoch should be non-zero and reasonably recent (within 1 day of now)
    time_t now = time(nullptr);
    TEST_ASSERT_TRUE(r.epoch > 0);
    int diff = abs((int)(now - r.epoch));
    TEST_ASSERT_TRUE(diff < 86400);  // within 24h (handles TZ parsing quirks)
}

// ── Reconnect tests ─────────────────────────────────────────────────────────

void test_reconnect_sets_apn(void) {
    // This test would have caught the missing APN bug.
    // After AT+CFUN=0 (radio off), SimModem clears apnSet.
    // modemReconnect() must send AT+CGDCONT before ATD*99#,
    // otherwise SimModem returns NO CARRIER.

    modemAtSync();
    ModemInitResult init = modemRunInit();
    TEST_ASSERT_TRUE(init.connected);

    // Simulate PPP drop — modemReconnect does the full sequence
    ModemReconnectResult rr = modemReconnect();
    TEST_ASSERT_TRUE(rr.registered);
    TEST_ASSERT_TRUE(rr.connected);  // would FAIL without AT+CGDCONT in reconnect
    TEST_ASSERT_TRUE(rr.rssi > 0 && rr.rssi < 99);
    TEST_ASSERT_TRUE(rr.operatorName[0] != '\0');
}

void test_reconnect_without_apn_fails(void) {
    // Verify SimModem enforces APN requirement
    modemAtSync();

    char resp[256];
    // Set APN and dial — should work
    modem_at_cmd("AT+CGDCONT=1,\"IP\",\"hologram\"", resp, sizeof(resp), 1000);
    modem_at_cmd("ATD*99#", resp, sizeof(resp), 2000);
    TEST_ASSERT_TRUE(strstr(resp, "CONNECT") != nullptr);

    // Escape back to command mode
    g_hal->clock->delay_ms(1100);
    mdm_write("+++", 3);
    g_hal->clock->delay_ms(1100);
    mdm_flush();
    usleep(600000);
    mdm_read(resp, sizeof(resp), 500);

    // Reset radio (clears APN)
    modem_at_cmd("AT+CFUN=0", resp, sizeof(resp), 2000);
    modem_at_cmd("AT+CFUN=1", resp, sizeof(resp), 2000);

    // Dial WITHOUT re-setting APN — should fail
    modem_at_cmd("ATD*99#", resp, sizeof(resp), 2000);
    TEST_ASSERT_TRUE(strstr(resp, "NO CARRIER") != nullptr);

    // Now set APN and dial — should work
    modem_at_cmd("AT+CGDCONT=1,\"IP\",\"hologram\"", resp, sizeof(resp), 1000);
    modem_at_cmd("ATD*99#", resp, sizeof(resp), 2000);
    TEST_ASSERT_TRUE(strstr(resp, "CONNECT") != nullptr);
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_at_sync);
    RUN_TEST(test_full_init);
    RUN_TEST(test_init_no_registration);
    RUN_TEST(test_init_roaming);
    RUN_TEST(test_init_weak_signal);
    RUN_TEST(test_init_different_operator);
    RUN_TEST(test_init_time_sync);

    // Reconnect
    RUN_TEST(test_reconnect_sets_apn);
    RUN_TEST(test_reconnect_without_apn_fails);

    return UNITY_END();
}

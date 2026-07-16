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
void mdm_set_flow_control(bool enable) { s_uart_ptr->set_flow_control(enable); }

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
    // Richer LTE metrics from SimModem defaults (rsrpIdx=70, rsrqIdx=26, sinr=12).
    TEST_ASSERT_EQUAL_INT(70 - 141, r.rsrp);     // -71 dBm
    TEST_ASSERT_EQUAL_INT((26 / 2) - 20, r.rsrq); // -7 dB
    TEST_ASSERT_EQUAL_INT(12, r.sinr);
    TEST_ASSERT_EQUAL_STRING("EUTRAN-BAND4", r.band);
}

void test_init_signal_metrics_custom(void) {
    s_modem->rsrpIdx = 30;   // -111 dBm (weak)
    s_modem->rsrqIdx = 10;   // -15 dB
    s_modem->sinr    = -2;
    s_modem->band    = "EUTRAN-BAND12";

    modemAtSync();
    ModemInitResult r = modemRunInit();
    TEST_ASSERT_EQUAL_INT(30 - 141, r.rsrp);      // -111 dBm
    TEST_ASSERT_EQUAL_INT((10 / 2) - 20, r.rsrq); // -15 dB
    TEST_ASSERT_EQUAL_INT(-2, r.sinr);
    TEST_ASSERT_EQUAL_STRING("EUTRAN-BAND12", r.band);
}

void test_survey_sample(void) {
    // The antenna survey reads CSQ+CESQ+CPSI+COPS without dialing PPP.
    s_modem->rssi = 20;
    s_modem->rsrpIdx = 65;   // -76 dBm
    s_modem->rsrqIdx = 24;   // -8 dB
    s_modem->sinr    = 9;
    s_modem->band    = "EUTRAN-BAND2";
    s_modem->operatorName = "Verizon";

    modemAtSync();
    ModemSignalSample s;
    modemSurveySample(&s);
    TEST_ASSERT_EQUAL_INT(20, s.rssi);
    TEST_ASSERT_EQUAL_INT(65 - 141, s.rsrp);       // -76 dBm
    TEST_ASSERT_EQUAL_INT((24 / 2) - 20, s.rsrq);  // -8 dB
    TEST_ASSERT_EQUAL_INT(9, s.sinr);
    TEST_ASSERT_EQUAL_STRING("EUTRAN-BAND2", s.band);
    TEST_ASSERT_EQUAL_STRING("Verizon", s.carrier);
}

// Pure parser tests (no modem needed).
void test_parse_cesq_valid(void) {
    int rsrp = 0, rsrq = 0;
    parseCesq("\r\n+CESQ: 99,99,255,255,26,70\r\n\r\nOK\r\n", &rsrp, &rsrq);
    TEST_ASSERT_EQUAL_INT(-71, rsrp);
    TEST_ASSERT_EQUAL_INT(-7, rsrq);
}

void test_parse_cesq_unavailable(void) {
    int rsrp = 0, rsrq = 0;
    parseCesq("+CESQ: 99,99,255,255,255,255", &rsrp, &rsrq);
    TEST_ASSERT_EQUAL_INT(MODEM_SIG_NA, rsrp);
    TEST_ASSERT_EQUAL_INT(MODEM_SIG_NA, rsrq);
}

void test_parse_cesq_malformed(void) {
    int rsrp = 0, rsrq = 0;
    parseCesq("garbage response OK", &rsrp, &rsrq);
    TEST_ASSERT_EQUAL_INT(MODEM_SIG_NA, rsrp);
    TEST_ASSERT_EQUAL_INT(MODEM_SIG_NA, rsrq);
}

void test_parse_cpsi_band_and_sinr(void) {
    char band[16] = "";
    int sinr = 0;
    parseCpsi("+CPSI: LTE,Online,310-260,0x1234,12345678,256,"
              "EUTRAN-BAND4,5110,5,5,-7,-71,-65,14\r\nOK", band, sizeof(band), &sinr);
    TEST_ASSERT_EQUAL_STRING("EUTRAN-BAND4", band);
    TEST_ASSERT_EQUAL_INT(14, sinr);
}

void test_parse_cpsi_absent(void) {
    char band[16] = "x";
    int sinr = 123;
    parseCpsi("NO SERVICE", band, sizeof(band), &sinr);
    TEST_ASSERT_EQUAL_STRING("", band);
    TEST_ASSERT_EQUAL_INT(MODEM_SIG_NA, sinr);
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

void test_init_csq99_retried(void) {
    // Real SIM7600: the first CSQ right after registration can read 99 even on
    // a strong link. Without the retry the bogus 99 stuck for the whole session
    // (2026-07-15 flight: rssi=99 all day, RSRP -74). One retry must recover it.
    s_modem->rssi = 21;
    s_modem->csq99Count = 1;   // first CSQ query answers 99, then real value

    modemAtSync();
    ModemInitResult r = modemRunInit();
    TEST_ASSERT_EQUAL_INT_MESSAGE(21, r.rssi, "retry must pick up the real RSSI");
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

void test_reconnect_reports_link_metrics(void) {
    // The landing reconnect must record WHAT link it got (band/RSRP/SINR) —
    // the 2026-07-15 flight log only had CSQ, leaving the descent link quality
    // unknowable. Metrics are read in command mode before the redial.
    s_modem->rsrpIdx = 45;   // -96 dBm
    s_modem->rsrqIdx = 20;   // -10 dB
    s_modem->sinr    = 5;
    s_modem->band    = "EUTRAN-BAND12";

    modemAtSync();
    ModemReconnectResult rr = modemReconnect(false);   // soft reconnect
    TEST_ASSERT_TRUE(rr.registered);
    TEST_ASSERT_EQUAL_INT(45 - 141, rr.rsrp);          // -96 dBm
    TEST_ASSERT_EQUAL_INT((20 / 2) - 20, rr.rsrq);     // -10 dB
    TEST_ASSERT_EQUAL_INT(5, rr.sinr);
    TEST_ASSERT_EQUAL_STRING("EUTRAN-BAND12", rr.band);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, rr.baud, "soft reconnect leaves UART untouched");
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

// ── Stranded-carrier recovery ────────────────────────────────────────────────

void test_stranded_carrier_recovered_by_cops(void) {
    // A unit that boots REGISTERED onto a DEAD carrier (the Hologram multi-IMSI SIM
    // camped on a no-service PLMN — the "T-Mobile, no bars, no data" field failure)
    // gets NO CARRIER on ATD*99# despite valid registration, and the initial-dial
    // loop would spin forever. The firmware's remedy is AT+COPS=0 (automatic operator
    // reselection). Verify the mechanism end-to-end against the sim.
    s_modem->regStat = 5;         // registered (roaming) …
    s_modem->stranded = true;     // … but on a dead carrier: no data path

    modemAtSync();
    ModemInitResult r = modemRunInit();
    TEST_ASSERT_TRUE(r.registered);    // registration is fine
    TEST_ASSERT_FALSE(r.connected);    // yet the dial fails (NO CARRIER) — stranded

    // Force automatic operator reselection — drops the dead PLMN.
    char resp[128];
    modem_at_cmd("AT+COPS=0", resp, sizeof(resp), 2000);
    TEST_ASSERT_TRUE(strstr(resp, "OK") != nullptr);
    TEST_ASSERT_FALSE(s_modem->stranded);   // reselected off the dead carrier

    // A redial now succeeds.
    ModemReconnectResult rr = modemReconnect();
    TEST_ASSERT_TRUE(rr.connected);
}

// ── Baud recovery after radio / factory resets ──────────────────────────────

void test_radio_reset_reconnect_restores_baud(void) {
    // A CFUN=0/1 radio reset drops the SIM7600 UART to 115200. The reconnect
    // must negotiate back to full speed itself — leaving it "for the next boot"
    // meant every post-reset session uploaded at ~10 KB/s instead of 73-167 KB/s
    // (the post-landing taxi-in window, after a flight full of failed attempts,
    // is exactly such a session).
    modemAtSync();
    ModemBaudResult br = modemUpgradeBaud();          // boot-time upgrade
    TEST_ASSERT_EQUAL_UINT32(3000000, br.baud);
    TEST_ASSERT_EQUAL_INT(3000000, s_modem->baudRate);

    ModemReconnectResult rr = modemReconnect(true);   // radio-reset reconnect
    TEST_ASSERT_TRUE(rr.registered);
    TEST_ASSERT_TRUE(rr.connected);
    TEST_ASSERT_EQUAL_INT_MESSAGE(3000000, s_modem->baudRate,
        "radio-reset reconnect must re-upgrade the baud, not stay at 115200");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(3000000, rr.baud,
        "result must report the renegotiated baud for the log");
}

void test_factory_reset_recover_full_speed(void) {
    // modemFactoryReset reboots the module to factory defaults (115200, echo on);
    // modemFactoryRecover must resync, turn echo back off, and re-upgrade baud.
    modemAtSync();
    modemUpgradeBaud();
    TEST_ASSERT_EQUAL_INT(3000000, s_modem->baudRate);

    modemFactoryReset();
    TEST_ASSERT_EQUAL_INT_MESSAGE(115200, s_modem->baudRate, "module rebooted to defaults");
    TEST_ASSERT_TRUE_MESSAGE(s_modem->echoEnabled, "factory default is echo on");

    TEST_ASSERT_TRUE(modemFactoryRecover());
    TEST_ASSERT_FALSE_MESSAGE(s_modem->echoEnabled, "recover re-applies ATE0");
    TEST_ASSERT_EQUAL_INT_MESSAGE(3000000, s_modem->baudRate,
        "recover re-upgrades to full speed");
}

// ── Signal-gated reconnect wait ──────────────────────────────────────────────

void test_wait_for_signal_returns_when_registered(void) {
    s_modem->regStat = 5;   // roaming = registered (LTE data)
    s_modem->rssi = 18;
    uint32_t t0 = s_clock.millis();
    ModemSignalWait sw = modemWaitForSignal(5000, 500);
    uint32_t dt = s_clock.millis() - t0;
    TEST_ASSERT_TRUE_MESSAGE(sw.registered, "registered network → returns registered");
    TEST_ASSERT_EQUAL_INT_MESSAGE(18, sw.rssi, "reads RSSI when registered");
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(500, dt, "returns immediately (no wasted poll wait)");
}

void test_wait_for_signal_times_out_when_unregistered(void) {
    s_modem->regStat = 2;   // searching, never registers
    uint32_t t0 = s_clock.millis();
    ModemSignalWait sw = modemWaitForSignal(300, 100);  // small for a fast test
    uint32_t dt = s_clock.millis() - t0;
    TEST_ASSERT_FALSE_MESSAGE(sw.registered, "no coverage → not registered");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32_MESSAGE(300, dt, "polled up to maxWait");
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(2000, dt, "but capped — didn't hang");
}

void test_wait_for_signal_reports_rssi_while_unregistered(void) {
    // CSQ is read on every poll, registered or not: a visible cell without
    // registration is the "stranded, not out-of-coverage" evidence that keeps
    // the factory-reset cadence aggressive (modemReconnectPlan signalSeen).
    s_modem->regStat = 2;   // searching, never registers…
    s_modem->rssi = 17;     // …but a cell is visible
    ModemSignalWait sw = modemWaitForSignal(300, 100);
    TEST_ASSERT_FALSE(sw.registered);
    TEST_ASSERT_EQUAL_INT_MESSAGE(17, sw.rssi, "visible-cell RSSI reported unregistered");

    s_modem->rssi = 99;     // deep out of coverage — nothing visible
    sw = modemWaitForSignal(300, 100);
    TEST_ASSERT_FALSE(sw.registered);
    TEST_ASSERT_EQUAL_INT_MESSAGE(99, sw.rssi, "no cells → rssi stays 99");
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_at_sync);
    RUN_TEST(test_full_init);
    RUN_TEST(test_init_no_registration);
    RUN_TEST(test_init_roaming);
    RUN_TEST(test_init_weak_signal);
    RUN_TEST(test_init_csq99_retried);
    RUN_TEST(test_init_different_operator);
    RUN_TEST(test_init_time_sync);
    RUN_TEST(test_init_signal_metrics_custom);
    RUN_TEST(test_survey_sample);
    RUN_TEST(test_parse_cesq_valid);
    RUN_TEST(test_parse_cesq_unavailable);
    RUN_TEST(test_parse_cesq_malformed);
    RUN_TEST(test_parse_cpsi_band_and_sinr);
    RUN_TEST(test_parse_cpsi_absent);

    // Reconnect
    RUN_TEST(test_reconnect_sets_apn);
    RUN_TEST(test_reconnect_reports_link_metrics);
    RUN_TEST(test_reconnect_without_apn_fails);
    RUN_TEST(test_stranded_carrier_recovered_by_cops);
    RUN_TEST(test_radio_reset_reconnect_restores_baud);
    RUN_TEST(test_factory_reset_recover_full_speed);
    RUN_TEST(test_wait_for_signal_returns_when_registered);
    RUN_TEST(test_wait_for_signal_times_out_when_unregistered);
    RUN_TEST(test_wait_for_signal_reports_rssi_while_unregistered);

    return UNITY_END();
}

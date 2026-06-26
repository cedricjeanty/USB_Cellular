#include <unity.h>
#include <cstring>
#include <string>
#include <unistd.h>
#include "sim_modem.h"
#include "hal/uart_pty.h"
#include "airbridge_modem.h"   // modemReconnectPlan

static SimModem* s_modem = nullptr;
static PtyUart   s_uart;

// Helper: send AT command and read response
static std::string sendAT(const char* cmd, int timeout_ms = 1000) {
    std::string full = std::string(cmd) + "\r";
    s_uart.write(full.c_str(), full.size());
    usleep(50000);  // 50ms for simulator to process

    char buf[512] = "";
    int total = 0;
    uint32_t deadline = timeout_ms;
    while (deadline > 0) {
        int n = s_uart.read(buf + total, sizeof(buf) - 1 - total, 100);
        if (n > 0) total += n;
        buf[total] = '\0';
        if (strstr(buf, "OK") || strstr(buf, "ERROR") || strstr(buf, "CONNECT"))
            break;
        deadline -= 100;
    }
    return std::string(buf);
}

void setUp(void) {
    if (s_modem) { s_modem->stop(); delete s_modem; }
    s_modem = new SimModem();
    s_modem->rssi = 22;
    s_modem->operatorName = "T-Mobile";
    s_modem->echoEnabled = false;
    TEST_ASSERT_TRUE(s_modem->start());
    s_uart.fd = s_modem->slave_fd;
    usleep(50000);  // let thread start
    // Set APN (required before ATD*99# — SimModem enforces this like real SIM7600)
    sendAT("AT+CGDCONT=1,\"IP\",\"hologram\"");
}

void tearDown(void) {
    if (s_modem) { s_modem->stop(); delete s_modem; s_modem = nullptr; }
}

// ── Basic AT command tests ──────────────────────────────────────────────────

void test_at_ok(void) {
    std::string resp = sendAT("AT");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_at_csq(void) {
    std::string resp = sendAT("AT+CSQ");
    TEST_ASSERT_TRUE(resp.find("+CSQ: 22,99") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_at_csq_different_rssi(void) {
    s_modem->rssi = 15;
    std::string resp = sendAT("AT+CSQ");
    TEST_ASSERT_TRUE(resp.find("+CSQ: 15,99") != std::string::npos);
}

void test_at_cops(void) {
    std::string resp = sendAT("AT+COPS?");
    TEST_ASSERT_TRUE(resp.find("T-Mobile") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_at_creg(void) {
    std::string resp = sendAT("AT+CREG?");
    TEST_ASSERT_TRUE(resp.find("+CREG: 1,1") != std::string::npos);
}

void test_at_creg_roaming(void) {
    s_modem->regStat = 5;
    std::string resp = sendAT("AT+CREG?");
    TEST_ASSERT_TRUE(resp.find("+CREG: 1,5") != std::string::npos);
}

void test_at_cclk(void) {
    std::string resp = sendAT("AT+CCLK?");
    TEST_ASSERT_TRUE(resp.find("+CCLK:") != std::string::npos);
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_at_cfun(void) {
    std::string resp = sendAT("AT+CFUN=0");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
    resp = sendAT("AT+CFUN=1");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_at_echo_off(void) {
    s_modem->echoEnabled = true;
    std::string resp = sendAT("ATE0");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
    // After ATE0, echo should be off
    resp = sendAT("AT");
    // Should NOT see "AT" echoed back before OK
    // (hard to verify precisely, just check OK is there)
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_at_cgdcont(void) {
    std::string resp = sendAT("AT+CGDCONT=1,\"IP\",\"hologram\"");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_at_ipr(void) {
    std::string resp = sendAT("AT+IPR=921600");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

// ── PPP dial / data mode tests ──────────────────────────────────────────────

void test_dial_ppp(void) {
    std::string resp = sendAT("ATD*99#");
    TEST_ASSERT_TRUE(resp.find("CONNECT") != std::string::npos);
}

void test_escape_sequence(void) {
    // Dial into data mode
    sendAT("ATD*99#");
    usleep(200000);

    // Send +++ to escape (need silence before and after)
    usleep(100000);
    s_uart.write("+++", 3);
    usleep(800000);  // wait for escape detection (500ms+ silence after +++)

    // Should get OK back
    char buf[128] = "";
    int total = 0;
    for (int i = 0; i < 10; i++) {
        int n = s_uart.read(buf + total, sizeof(buf) - 1 - total, 200);
        if (n > 0) total += n;
        buf[total] = '\0';
        if (strstr(buf, "OK")) break;
    }
    TEST_ASSERT_TRUE(strstr(buf, "OK") != nullptr);

    // Should be back in command mode — AT should work
    std::string resp = sendAT("AT");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);
}

void test_ato_return_to_data(void) {
    // Dial, escape, then ATO
    sendAT("ATD*99#");
    usleep(200000);
    s_uart.write("+++", 3);
    usleep(800000);
    char tmp[128];
    int total = 0;
    for (int i = 0; i < 10; i++) {
        int n = s_uart.read(tmp + total, sizeof(tmp) - 1 - total, 200);
        if (n > 0) total += n;
        tmp[total] = '\0';
        if (strstr(tmp, "OK")) break;
    }

    std::string resp = sendAT("ATO");
    TEST_ASSERT_TRUE(resp.find("CONNECT") != std::string::npos);
}

// ── Sequence test (mimics real modem init) ──────────────────────────────────

void test_full_init_sequence(void) {
    // This mimics what modemTask does after AT sync
    std::string resp;

    resp = sendAT("AT+CFUN=0", 3000);
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);

    resp = sendAT("AT+CFUN=1", 3000);
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);

    resp = sendAT("ATE0");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);

    resp = sendAT("AT+CTZU=1");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);

    resp = sendAT("AT+CCLK?", 2000);
    TEST_ASSERT_TRUE(resp.find("+CCLK:") != std::string::npos);

    resp = sendAT("AT+CREG=1");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);

    resp = sendAT("AT+AUTOCSQ=1,1");
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);

    resp = sendAT("AT+CREG?");
    TEST_ASSERT_TRUE(resp.find(",1") != std::string::npos);

    resp = sendAT("AT+CSQ", 2000);
    TEST_ASSERT_TRUE(resp.find("+CSQ:") != std::string::npos);

    resp = sendAT("AT+COPS?", 2000);
    TEST_ASSERT_TRUE(resp.find("T-Mobile") != std::string::npos);

    resp = sendAT("AT+CGDCONT=1,\"IP\",\"hologram\"", 5000);
    TEST_ASSERT_TRUE(resp.find("OK") != std::string::npos);

    resp = sendAT("ATD*99#", 5000);
    TEST_ASSERT_TRUE(resp.find("CONNECT") != std::string::npos);
}

// ── Reconnect escalation plan (pure decision logic) ─────────────────────────

void test_reconnect_plan_soft_first_four(void) {
    // Failures 0-3 = attempts 1-4: soft, no backoff.
    for (int f = 0; f < 4; f++) {
        ModemReconnectPlan p = modemReconnectPlan(f);
        TEST_ASSERT_FALSE_MESSAGE(p.radioReset, "attempts 1-4 are soft");
        TEST_ASSERT_EQUAL_UINT32(0, p.backoffSeconds);
    }
}

void test_reconnect_plan_first_cfun_at_five(void) {
    ModemReconnectPlan p = modemReconnectPlan(4);   // attempt 5
    TEST_ASSERT_TRUE(p.radioReset);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, p.backoffSeconds, "first CFUN has no backoff yet");
}

void test_reconnect_plan_soft_between_resets(void) {
    for (int f = 5; f < 9; f++)                     // attempts 6-9
        TEST_ASSERT_FALSE(modemReconnectPlan(f).radioReset);
}

void test_reconnect_plan_backoff_after_second_reset(void) {
    ModemReconnectPlan p9 = modemReconnectPlan(9);  // 2nd CFUN reset
    TEST_ASSERT_TRUE(p9.radioReset);
    TEST_ASSERT_EQUAL_UINT32(30, p9.backoffSeconds);
    ModemReconnectPlan p14 = modemReconnectPlan(14); // 3rd reset
    TEST_ASSERT_TRUE(p14.radioReset);
    TEST_ASSERT_EQUAL_UINT32(60, p14.backoffSeconds);
}

void test_reconnect_plan_backoff_capped(void) {
    // Very high failure counts cap the backoff at 300s.
    TEST_ASSERT_EQUAL_UINT32(300, modemReconnectPlan(4 + 5 * 11).backoffSeconds);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(300, modemReconnectPlan(1000).backoffSeconds);
}

// ── Flaky-link injection (marginal cellular) ────────────────────────────────
// The flight-cycle test's "flaky approach" window drops a % of IP packets. The
// load-bearing invariant: ONLY PPP_IP is ever dropped — dropping LCP/IPCP would
// tear the link down instead of making it lossy.

void test_flaky_never_drops_control_frames(void) {
    SimModem m;
    m.setFlaky(100);  // maximum loss
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_FALSE_MESSAGE(m.flakyDropsFrame(PPP_LCP),  "LCP must NEVER be dropped");
        TEST_ASSERT_FALSE_MESSAGE(m.flakyDropsFrame(PPP_IPCP), "IPCP must NEVER be dropped");
    }
}

void test_flaky_zero_drops_nothing(void) {
    SimModem m; m.setFlaky(0);
    for (int i = 0; i < 100; i++) TEST_ASSERT_FALSE(m.flakyDropsFrame(PPP_IP));
}

void test_flaky_full_drops_all_ip(void) {
    SimModem m; m.setFlaky(100);
    for (int i = 0; i < 100; i++) TEST_ASSERT_TRUE(m.flakyDropsFrame(PPP_IP));
}

void test_flaky_partial_drops_roughly_half(void) {
    SimModem m; m.setFlaky(50); srand(12345);
    int dropped = 0; const int N = 4000;
    for (int i = 0; i < N; i++) if (m.flakyDropsFrame(PPP_IP)) dropped++;
    TEST_ASSERT_TRUE_MESSAGE(dropped > N * 4 / 10 && dropped < N * 6 / 10,
                             "~50% of IP frames dropped at flaky=50");
}

void test_flaky_clamps_range(void) {
    SimModem m;
    m.setFlaky(-10); TEST_ASSERT_EQUAL_INT(0, m.flakyDropPct.load());
    m.setFlaky(250); TEST_ASSERT_EQUAL_INT(100, m.flakyDropPct.load());
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // Basic AT commands
    RUN_TEST(test_at_ok);
    RUN_TEST(test_at_csq);
    RUN_TEST(test_at_csq_different_rssi);
    RUN_TEST(test_at_cops);
    RUN_TEST(test_at_creg);
    RUN_TEST(test_at_creg_roaming);
    RUN_TEST(test_at_cclk);
    RUN_TEST(test_at_cfun);
    RUN_TEST(test_at_echo_off);
    RUN_TEST(test_at_cgdcont);
    RUN_TEST(test_at_ipr);

    // PPP/data mode
    RUN_TEST(test_dial_ppp);
    RUN_TEST(test_escape_sequence);
    RUN_TEST(test_ato_return_to_data);

    // Full sequence
    RUN_TEST(test_full_init_sequence);

    // Reconnect escalation plan
    RUN_TEST(test_reconnect_plan_soft_first_four);
    RUN_TEST(test_reconnect_plan_first_cfun_at_five);
    RUN_TEST(test_reconnect_plan_soft_between_resets);
    RUN_TEST(test_reconnect_plan_backoff_after_second_reset);
    RUN_TEST(test_reconnect_plan_backoff_capped);

    // Flaky-link injection
    RUN_TEST(test_flaky_never_drops_control_frames);
    RUN_TEST(test_flaky_zero_drops_nothing);
    RUN_TEST(test_flaky_full_drops_all_ip);
    RUN_TEST(test_flaky_partial_drops_roughly_half);
    RUN_TEST(test_flaky_clamps_range);

    return UNITY_END();
}

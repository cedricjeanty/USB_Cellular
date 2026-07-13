#include <unity.h>
#include <cstring>
#include <string>
#include <unistd.h>
#include "sim_modem.h"
#include "hal/uart_pty.h"
#include "hal/test_impls.h"
#include "airbridge_modem.h"   // modemReconnectPlan, modemBoundedTx

// modemBoundedTx uses g_hal->clock; give this TU a controllable one.
static StubDisplay s_btxDisplay;
static TestClock   s_btxClock;
static HAL         s_btxHal = { &s_btxDisplay, &s_btxClock, nullptr, nullptr, nullptr, nullptr };
HAL* g_hal = nullptr;

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
    // Failures 0-3 = attempts 1-4: soft, no backoff (regardless of signal evidence).
    for (int f = 0; f < 4; f++) {
        for (int sig = 0; sig <= 1; sig++) {
            ModemReconnectPlan p = modemReconnectPlan(f, sig != 0);
            TEST_ASSERT_FALSE_MESSAGE(p.radioReset, "attempts 1-4 are soft");
            TEST_ASSERT_EQUAL_UINT32(0, p.backoffSeconds);
        }
    }
}

void test_reconnect_plan_first_cfun_at_five(void) {
    ModemReconnectPlan p = modemReconnectPlan(4, true);   // attempt 5
    TEST_ASSERT_TRUE(p.radioReset);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(10, p.backoffSeconds, "radio reset has a short 10s settle");
    // Same with no signal evidence — CFUN resets are not signal-gated.
    TEST_ASSERT_TRUE(modemReconnectPlan(4, false).radioReset);
}

void test_reconnect_plan_soft_between_resets(void) {
    for (int f = 5; f < 9; f++)                     // attempts 6-9
        TEST_ASSERT_FALSE(modemReconnectPlan(f, true).radioReset);
}

void test_reconnect_plan_backoff_is_flat_short(void) {
    // No escalating/300s backoff anymore — a radio reset gets a flat 10s settle, soft
    // attempts get 0; the reconnect loop is signal-gated (poll every ~10s), so we never
    // burn minutes idle. Every radio-reset failure count → exactly 10s.
    ModemReconnectPlan p9  = modemReconnectPlan(9, true);          // 2nd reset
    ModemReconnectPlan p14 = modemReconnectPlan(14, true);         // 3rd reset
    ModemReconnectPlan pHi = modemReconnectPlan(4 + 5 * 11, true); // very high
    TEST_ASSERT_TRUE(p9.radioReset && p14.radioReset && pHi.radioReset);
    TEST_ASSERT_EQUAL_UINT32(10, p9.backoffSeconds);
    TEST_ASSERT_EQUAL_UINT32(10, p14.backoffSeconds);
    TEST_ASSERT_EQUAL_UINT32(10, pHi.backoffSeconds);
    // Soft attempts never wait, and nothing ever exceeds the 10s settle.
    TEST_ASSERT_EQUAL_UINT32(0, modemReconnectPlan(1000, true).backoffSeconds);  // 1000: not a reset
    for (int f = 0; f < 200; f++) {
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(10, modemReconnectPlan(f, true).backoffSeconds);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(10, modemReconnectPlan(f, false).backoffSeconds);
    }
}

void test_reconnect_plan_factory_reset_escalation(void) {
    // Deepest tier with signal evidence (cells visible yet no service = genuinely
    // stranded): full factory reset at 12, 24, 36... It supersedes the radio reset
    // on those attempts.
    ModemReconnectPlan p12 = modemReconnectPlan(12, true);
    TEST_ASSERT_TRUE_MESSAGE(p12.factoryReset, "factory reset at 12 failures");
    TEST_ASSERT_FALSE_MESSAGE(p12.radioReset, "factory reset supersedes radio reset");
    TEST_ASSERT_TRUE(modemReconnectPlan(24, true).factoryReset);
    TEST_ASSERT_TRUE(modemReconnectPlan(36, true).factoryReset);
    // Not on the radio-reset / soft attempts.
    TEST_ASSERT_FALSE(modemReconnectPlan(4, true).factoryReset);
    TEST_ASSERT_FALSE(modemReconnectPlan(9, true).factoryReset);
    TEST_ASSERT_FALSE(modemReconnectPlan(14, true).factoryReset);
    TEST_ASSERT_FALSE(modemReconnectPlan(0, true).factoryReset);
    // 4/9/14 stay radio resets (factory reset only at the 12-multiples).
    TEST_ASSERT_TRUE(modemReconnectPlan(4, true).radioReset && modemReconnectPlan(9, true).radioReset);
}

void test_reconnect_plan_factory_reset_gated_on_signal(void) {
    // No signal evidence at all since the last good session = most likely airborne
    // out of coverage, where a factory reset can't help (and being mid-reset at
    // touchdown delays the taxi-in upload). Cadence backs off to every 36th failure
    // — kept (not removed) so a unit band-locked onto a dead band still recovers.
    TEST_ASSERT_FALSE_MESSAGE(modemReconnectPlan(12, false).factoryReset,
                              "no factory reset at 12 without signal evidence");
    TEST_ASSERT_FALSE(modemReconnectPlan(24, false).factoryReset);
    TEST_ASSERT_TRUE_MESSAGE(modemReconnectPlan(36, false).factoryReset,
                             "recovery guarantee: factory reset still fires at 36");
    TEST_ASSERT_TRUE(modemReconnectPlan(72, false).factoryReset);
    // The periodic CFUN radio resets are NOT signal-gated — a no-coverage outage
    // still gets its every-5th radio reset (cheap, clears wedged radio state).
    TEST_ASSERT_TRUE(modemReconnectPlan(24, false).radioReset);   // (24-4)%5==0
    TEST_ASSERT_TRUE(modemReconnectPlan(14, false).radioReset);
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

// ── Bounded TX (modemBoundedTx) ─────────────────────────────────────────────
// The 2026-07-07 hardware wedge: uart_write_bytes blocked forever on a CTS-
// stalled UART during reconnect. modemBoundedTx is the replacement: push what
// the FIFO accepts, give up at the deadline, report a timeout for escalation.

struct PushScript {
    size_t capTotal;      // accept at most this many bytes total (SIZE_MAX = all)
    int    zerosFirst;    // return 0 (FIFO full) this many calls before accepting
    bool   error;         // return -1 always
    size_t accepted = 0;
};
static int scriptedPush(const void* p, size_t n, void* ctx) {
    (void)p;
    PushScript* s = (PushScript*)ctx;
    if (s->error) return -1;
    if (s->zerosFirst > 0) { s->zerosFirst--; return 0; }
    size_t room = (s->capTotal > s->accepted) ? s->capTotal - s->accepted : 0;
    size_t take = (n < room) ? n : room;
    s->accepted += take;
    return (int)take;
}

void test_bounded_tx_fast_path(void) {
    g_hal = &s_btxHal; s_btxClock.now_ms = 1000;
    PushScript s = { SIZE_MAX, 0, false };
    ModemTxOps ops = { scriptedPush, &s };
    ModemTxResult r = modemBoundedTx(ops, "ATD*99#\r", 8, 2000);
    TEST_ASSERT_FALSE(r.timedOut);
    TEST_ASSERT_EQUAL_UINT32(8, (uint32_t)r.written);
    TEST_ASSERT_EQUAL_UINT32(1000, s_btxClock.now_ms);  // no waiting on the fast path
}

void test_bounded_tx_stalled_fifo_times_out(void) {
    // FIFO accepts 7 bytes then never drains (CTS held low) — must return at
    // the deadline with a timeout flag, never block forever.
    g_hal = &s_btxHal; s_btxClock.now_ms = 0;
    PushScript s = { 7, 0, false };
    ModemTxOps ops = { scriptedPush, &s };
    uint8_t frame[64] = {0};
    ModemTxResult r = modemBoundedTx(ops, frame, sizeof(frame), 2000);
    TEST_ASSERT_TRUE(r.timedOut);
    TEST_ASSERT_EQUAL_UINT32(7, (uint32_t)r.written);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2000, s_btxClock.now_ms);   // gave it the full deadline
    TEST_ASSERT_LESS_THAN_UINT32(2100, s_btxClock.now_ms);          // ...but not much more
}

void test_bounded_tx_recovers_after_brief_stall(void) {
    // FIFO full for a few polls, then drains — completes without a timeout.
    g_hal = &s_btxHal; s_btxClock.now_ms = 0;
    PushScript s = { SIZE_MAX, 3, false };
    ModemTxOps ops = { scriptedPush, &s };
    uint8_t frame[64] = {0};
    ModemTxResult r = modemBoundedTx(ops, frame, sizeof(frame), 2000);
    TEST_ASSERT_FALSE(r.timedOut);
    TEST_ASSERT_EQUAL_UINT32(sizeof(frame), (uint32_t)r.written);
}

void test_bounded_tx_push_error_aborts(void) {
    g_hal = &s_btxHal; s_btxClock.now_ms = 0;
    PushScript s = { 0, 0, true };
    ModemTxOps ops = { scriptedPush, &s };
    ModemTxResult r = modemBoundedTx(ops, "AT\r", 3, 2000);
    TEST_ASSERT_TRUE(r.timedOut);
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)r.written);
    TEST_ASSERT_EQUAL_UINT32(0, s_btxClock.now_ms);  // immediate, no deadline burn
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
    RUN_TEST(test_reconnect_plan_backoff_is_flat_short);
    RUN_TEST(test_reconnect_plan_factory_reset_escalation);
    RUN_TEST(test_reconnect_plan_factory_reset_gated_on_signal);

    // Flaky-link injection
    RUN_TEST(test_flaky_never_drops_control_frames);
    RUN_TEST(test_flaky_zero_drops_nothing);
    RUN_TEST(test_flaky_full_drops_all_ip);
    RUN_TEST(test_flaky_partial_drops_roughly_half);
    RUN_TEST(test_flaky_clamps_range);

    // Bounded TX (CTS-stall wedge fix)
    RUN_TEST(test_bounded_tx_fast_path);
    RUN_TEST(test_bounded_tx_stalled_fifo_times_out);
    RUN_TEST(test_bounded_tx_recovers_after_brief_stall);
    RUN_TEST(test_bounded_tx_push_error_aborts);

    return UNITY_END();
}

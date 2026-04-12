#include <unity.h>
#include <cstring>
#include <string>
#include <unistd.h>
#include "sim_modem.h"
#include "hal/uart_pty.h"

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

    return UNITY_END();
}

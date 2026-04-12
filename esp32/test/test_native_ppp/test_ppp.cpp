// Tests for PPP protocol implementation and URC parser.
// Tests ppp_proto.h (frame build/parse, FCS) and UrcState (data pump extraction).
// Tests SimModem PPP negotiation: LCP + IPCP via real PTY.

#include <unity.h>
#include <cstring>
#include <unistd.h>
#include "ppp_proto.h"
#include "sim_modem.h"
#include "hal/uart_pty.h"

void setUp(void) {}
void tearDown(void) {}

// ── FCS tests ───────────────────────────────────────────────────────────────

void test_fcs_deterministic(void) {
    uint8_t data[] = { 0xFF, 0x03, 0xC0, 0x21 };
    uint16_t fcs1 = ppp_fcs(data, 4);
    uint16_t fcs2 = ppp_fcs(data, 4);
    TEST_ASSERT_EQUAL_UINT16(fcs1, fcs2);
}

void test_fcs_different_data(void) {
    uint8_t a[] = { 0xFF, 0x03, 0xC0, 0x21 };
    uint8_t b[] = { 0xFF, 0x03, 0x80, 0x21 };
    TEST_ASSERT_NOT_EQUAL(ppp_fcs(a, 4), ppp_fcs(b, 4));
}

// ── Frame build/parse roundtrip ─────────────────────────────────────────────

void test_frame_roundtrip_lcp(void) {
    // Build an LCP Configure-Request
    uint8_t payload[] = { PPP_CONF_REQ, 0x01, 0x00, 0x04 };  // code, id, length
    auto frame = ppp_build_frame(PPP_LCP, payload, sizeof(payload));

    TEST_ASSERT_EQUAL_UINT8(0x7E, frame.front());
    TEST_ASSERT_EQUAL_UINT8(0x7E, frame.back());

    // Parse it back
    uint16_t proto;
    std::vector<uint8_t> parsed;
    TEST_ASSERT_TRUE(ppp_parse_frame(frame.data(), frame.size(), &proto, &parsed));
    TEST_ASSERT_EQUAL_UINT16(PPP_LCP, proto);
    TEST_ASSERT_EQUAL_INT(sizeof(payload), parsed.size());
    TEST_ASSERT_EQUAL_UINT8(PPP_CONF_REQ, parsed[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, parsed[1]);
}

void test_frame_roundtrip_ipcp(void) {
    uint8_t payload[] = { PPP_CONF_REQ, 0x01, 0x00, 0x0A,
                          IPCP_OPT_IP_ADDR, 6, 0, 0, 0, 0 };  // request 0.0.0.0
    auto frame = ppp_build_frame(PPP_IPCP, payload, sizeof(payload));

    uint16_t proto;
    std::vector<uint8_t> parsed;
    TEST_ASSERT_TRUE(ppp_parse_frame(frame.data(), frame.size(), &proto, &parsed));
    TEST_ASSERT_EQUAL_UINT16(PPP_IPCP, proto);
    TEST_ASSERT_EQUAL_UINT8(IPCP_OPT_IP_ADDR, parsed[4]);
}

void test_frame_roundtrip_ip(void) {
    uint8_t ip_packet[] = { 0x45, 0x00, 0x00, 0x1C };  // IP header start
    auto frame = ppp_build_frame(PPP_IP, ip_packet, sizeof(ip_packet));

    uint16_t proto;
    std::vector<uint8_t> parsed;
    TEST_ASSERT_TRUE(ppp_parse_frame(frame.data(), frame.size(), &proto, &parsed));
    TEST_ASSERT_EQUAL_UINT16(PPP_IP, proto);
    TEST_ASSERT_EQUAL_UINT8(0x45, parsed[0]);
}

void test_frame_bad_fcs(void) {
    auto frame = ppp_build_frame(PPP_LCP, nullptr, 0);
    // Corrupt a byte
    frame[3] ^= 0xFF;
    uint16_t proto;
    std::vector<uint8_t> parsed;
    TEST_ASSERT_FALSE(ppp_parse_frame(frame.data(), frame.size(), &proto, &parsed));
}

void test_frame_byte_stuffing(void) {
    // Build a frame with bytes that need stuffing (0x7E, 0x7D, <0x20)
    uint8_t payload[] = { PPP_CONF_REQ, 0x01, 0x00, 0x06, 0x7E, 0x7D };
    auto frame = ppp_build_frame(PPP_LCP, payload, sizeof(payload));

    // Should contain escape sequences
    bool has_escape = false;
    for (size_t i = 1; i < frame.size() - 1; i++)
        if (frame[i] == 0x7D) has_escape = true;
    TEST_ASSERT_TRUE(has_escape);

    // But should still parse correctly
    uint16_t proto;
    std::vector<uint8_t> parsed;
    TEST_ASSERT_TRUE(ppp_parse_frame(frame.data(), frame.size(), &proto, &parsed));
    TEST_ASSERT_EQUAL_UINT8(0x7E, parsed[4]);
    TEST_ASSERT_EQUAL_UINT8(0x7D, parsed[5]);
}

// ── URC parser tests ────────────────────────────────────────────────────────

void test_urc_csq(void) {
    UrcState urc;
    urc.reset();
    const char* data = "+CSQ: 18,99\r\n";
    for (int i = 0; data[i]; i++) urc.feed(data[i]);
    TEST_ASSERT_EQUAL_INT(18, urc.rssi);
}

void test_urc_creg_home(void) {
    UrcState urc;
    urc.reset();
    const char* data = "+CREG: 1\r\n";
    for (int i = 0; data[i]; i++) urc.feed(data[i]);
    TEST_ASSERT_EQUAL_INT(1, urc.regStat);
    TEST_ASSERT_FALSE(urc.regLost);
}

void test_urc_creg_lost(void) {
    UrcState urc;
    urc.reset();
    const char* data = "+CREG: 0\r\n";
    for (int i = 0; data[i]; i++) urc.feed(data[i]);
    TEST_ASSERT_EQUAL_INT(0, urc.regStat);
    TEST_ASSERT_TRUE(urc.regLost);
}

void test_urc_between_ppp_frames(void) {
    UrcState urc;
    urc.reset();
    // PPP frame boundary, then URC, then another frame
    uint8_t data[] = { 0x7E, '+', 'C', 'S', 'Q', ':', ' ', '2', '5', ',', '9', '9', '\r', '\n', 0x7E };
    for (size_t i = 0; i < sizeof(data); i++) urc.feed(data[i]);
    TEST_ASSERT_EQUAL_INT(25, urc.rssi);
}

void test_urc_ignores_binary(void) {
    UrcState urc;
    urc.reset();
    // Binary PPP data (non-printable) should not accumulate
    uint8_t data[] = { 0x7E, 0xFF, 0x03, 0xC0, 0x21, 0x7E };
    for (size_t i = 0; i < sizeof(data); i++) urc.feed(data[i]);
    TEST_ASSERT_EQUAL_INT(99, urc.rssi);  // unchanged
}

// ── SimModem PPP negotiation (live over PTY) ────────────────────────────────

static SimModem* mdm = nullptr;
static PtyUart uart;

// Forward declaration
static std::string sendAT(const char* cmd);

static void setupModem() {
    if (mdm) { mdm->stop(); delete mdm; }
    mdm = new SimModem();
    mdm->echoEnabled = false;
    mdm->start();
    uart.fd = mdm->slave_fd;
    usleep(50000);
    // Set APN (required before ATD*99# — SimModem enforces this like real SIM7600)
    sendAT("AT+CGDCONT=1,\"IP\",\"hologram\"");
}

static void teardownModem() {
    if (mdm) { mdm->stop(); delete mdm; mdm = nullptr; }
}

// Helper: send AT command and read response
static std::string sendAT(const char* cmd) {
    std::string full = std::string(cmd) + "\r";
    uart.write(full.c_str(), full.size());
    usleep(50000);
    char buf[256] = "";
    int total = 0;
    for (int i = 0; i < 10; i++) {
        int n = uart.read(buf + total, sizeof(buf) - 1 - total, 100);
        if (n > 0) total += n;
        buf[total] = '\0';
        if (strstr(buf, "OK") || strstr(buf, "CONNECT")) break;
    }
    return std::string(buf);
}

void test_ppp_lcp_negotiation(void) {
    setupModem();

    // Dial into data mode
    std::string resp = sendAT("ATD*99#");
    TEST_ASSERT_TRUE(resp.find("CONNECT") != std::string::npos);
    usleep(500000);  // let SimModem settle into data mode

    // Send an LCP Configure-Request
    uint8_t lcpReq[] = { PPP_CONF_REQ, 0x01, 0x00, 0x04 };
    auto frame = ppp_build_frame(PPP_LCP, lcpReq, sizeof(lcpReq));
    int wn = ::write(mdm->slave_fd, frame.data(), frame.size());

    // Read responses directly from slave fd
    uint8_t buf[2048];
    int total = 0;
    for (int attempt = 0; attempt < 20; attempt++) {
        struct pollfd pfd = { mdm->slave_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 100) > 0) {
            int n = ::read(mdm->slave_fd, buf + total, sizeof(buf) - total);
            if (n > 0) total += n;
        }
        if (total > 0 && attempt > 5) break;
    }
    // Show what we got for debugging
    char msg[128];
    snprintf(msg, sizeof(msg), "Got %d bytes, first 8: %02X %02X %02X %02X %02X %02X %02X %02X",
             total,
             total > 0 ? buf[0] : 0, total > 1 ? buf[1] : 0,
             total > 2 ? buf[2] : 0, total > 3 ? buf[3] : 0,
             total > 4 ? buf[4] : 0, total > 5 ? buf[5] : 0,
             total > 6 ? buf[6] : 0, total > 7 ? buf[7] : 0);
    TEST_ASSERT_TRUE_MESSAGE(total > 0, msg);

    // Scan for LCP Configure-Ack
    bool gotAck = false;
    for (int i = 0; i < total; i++) {
        if (buf[i] == 0x7E) {
            uint16_t proto;
            std::vector<uint8_t> payload;
            if (ppp_parse_frame(buf + i, total - i, &proto, &payload)) {
                if (proto == PPP_LCP && payload.size() >= 1 && payload[0] == PPP_CONF_ACK) {
                    gotAck = true;
                }
            }
        }
    }
    TEST_ASSERT_TRUE(gotAck);

    teardownModem();
}

void test_ppp_ipcp_assigns_ip(void) {
    setupModem();
    sendAT("ATD*99#");
    usleep(100000);

    // LCP first (required before IPCP)
    uint8_t lcpReq[] = { PPP_CONF_REQ, 0x01, 0x00, 0x04 };
    auto lcpFrame = ppp_build_frame(PPP_LCP, lcpReq, sizeof(lcpReq));
    uart.write(lcpFrame.data(), lcpFrame.size());
    usleep(200000);

    // Drain LCP response
    uint8_t drain[1024];
    uart.read(drain, sizeof(drain), 300);

    // ACK SimModem's LCP Configure-Request
    // (SimModem sends its own Conf-Req after we ACK ours)
    // For simplicity, just ACK any LCP Conf-Req we see
    uint8_t lcpAck[] = { PPP_CONF_ACK, 0x01, 0x00, 0x04 };
    auto ackFrame = ppp_build_frame(PPP_LCP, lcpAck, sizeof(lcpAck));
    uart.write(ackFrame.data(), ackFrame.size());
    usleep(200000);

    // Send IPCP Configure-Request with IP 0.0.0.0 (request assignment)
    uint8_t ipcpReq[] = { PPP_CONF_REQ, 0x01, 0x00, 0x0A,
                          IPCP_OPT_IP_ADDR, 6, 0, 0, 0, 0 };
    auto ipcpFrame = ppp_build_frame(PPP_IPCP, ipcpReq, sizeof(ipcpReq));
    uart.write(ipcpFrame.data(), ipcpFrame.size());
    usleep(300000);

    // Read responses
    uint8_t buf[2048];
    int n = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        int r = uart.read(buf + n, sizeof(buf) - n, 200);
        if (r > 0) n += r;
    }
    TEST_ASSERT_TRUE(n > 0);

    bool gotNakWithIp = false;
    for (int i = 0; i < n; i++) {
        if (buf[i] == 0x7E) {
            uint16_t proto;
            std::vector<uint8_t> payload;
            if (ppp_parse_frame(buf + i, n - i, &proto, &payload)) {
                if (proto == PPP_IPCP && payload.size() >= 10 && payload[0] == PPP_CONF_NAK) {
                    // Check assigned IP is 10.64.64.2
                    if (payload[6] == 10 && payload[7] == 64 && payload[8] == 64 && payload[9] == 2) {
                        gotNakWithIp = true;
                    }
                }
            }
        }
    }
    TEST_ASSERT_TRUE(gotNakWithIp);

    teardownModem();
}

void test_ppp_echo_reply(void) {
    setupModem();
    sendAT("ATD*99#");
    usleep(100000);

    // Do LCP first
    uint8_t lcpReq[] = { PPP_CONF_REQ, 0x01, 0x00, 0x04 };
    auto frame = ppp_build_frame(PPP_LCP, lcpReq, sizeof(lcpReq));
    uart.write(frame.data(), frame.size());
    usleep(200000);
    uint8_t drain[512];
    uart.read(drain, sizeof(drain), 300);

    // Send LCP Echo-Request
    uint8_t echoReq[] = { PPP_ECHO_REQ, 0x42, 0x00, 0x08, 0xAA, 0xBB, 0xCC, 0xDD };
    auto echoFrame = ppp_build_frame(PPP_LCP, echoReq, sizeof(echoReq));
    uart.write(echoFrame.data(), echoFrame.size());
    usleep(200000);

    uint8_t buf[2048];
    int n = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        int r = uart.read(buf + n, sizeof(buf) - n, 200);
        if (r > 0) n += r;
    }
    TEST_ASSERT_TRUE(n > 0);

    bool gotEchoReply = false;
    for (int i = 0; i < n; i++) {
        if (buf[i] == 0x7E) {
            uint16_t proto;
            std::vector<uint8_t> payload;
            if (ppp_parse_frame(buf + i, n - i, &proto, &payload)) {
                if (proto == PPP_LCP && payload.size() >= 1 && payload[0] == PPP_ECHO_REP) {
                    TEST_ASSERT_EQUAL_UINT8(0x42, payload[1]);  // same ID
                    gotEchoReply = true;
                }
            }
        }
    }
    TEST_ASSERT_TRUE(gotEchoReply);

    teardownModem();
}

void test_ppp_full_negotiation(void) {
    setupModem();
    sendAT("ATD*99#");
    usleep(500000);

    // === LCP negotiation ===
    // Send our LCP Conf-Req
    uint8_t lcpReq[] = { PPP_CONF_REQ, 0x01, 0x00, 0x04 };
    auto frame = ppp_build_frame(PPP_LCP, lcpReq, sizeof(lcpReq));
    ::write(mdm->slave_fd, frame.data(), frame.size());
    usleep(300000);

    // Read and ACK SimModem's LCP Conf-Req
    uint8_t buf[2048];
    int total = 0;
    for (int i = 0; i < 10; i++) {
        struct pollfd pfd = { mdm->slave_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 100) > 0) {
            int n = ::read(mdm->slave_fd, buf + total, sizeof(buf) - total);
            if (n > 0) total += n;
        }
    }

    // Find and ACK SimModem's LCP Conf-Req
    for (int i = 0; i < total; i++) {
        if (buf[i] == 0x7E) {
            uint16_t proto;
            std::vector<uint8_t> payload;
            if (ppp_parse_frame(buf + i, total - i, &proto, &payload)) {
                if (proto == PPP_LCP && payload.size() >= 1 && payload[0] == PPP_CONF_REQ) {
                    auto ack = ppp_build_conf_ack(payload[1], payload.data() + 4, payload.size() - 4);
                    auto ackFrame = ppp_build_frame(PPP_LCP, ack.data(), ack.size());
                    ::write(mdm->slave_fd, ackFrame.data(), ackFrame.size());
                }
            }
        }
    }
    usleep(200000);

    // === IPCP negotiation ===
    // Request IP 0.0.0.0 → SimModem NAKs with 10.64.64.2
    uint8_t ipcpReq1[] = { PPP_CONF_REQ, 0x01, 0x00, 0x0A,
                           IPCP_OPT_IP_ADDR, 6, 0, 0, 0, 0 };
    auto ipcpFrame1 = ppp_build_frame(PPP_IPCP, ipcpReq1, sizeof(ipcpReq1));
    ::write(mdm->slave_fd, ipcpFrame1.data(), ipcpFrame1.size());
    usleep(300000);

    // Drain NAK + SimModem's IPCP Conf-Req
    total = 0;
    for (int i = 0; i < 10; i++) {
        struct pollfd pfd = { mdm->slave_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 100) > 0) {
            int n = ::read(mdm->slave_fd, buf + total, sizeof(buf) - total);
            if (n > 0) total += n;
        }
    }

    // ACK SimModem's IPCP Conf-Req
    for (int i = 0; i < total; i++) {
        if (buf[i] == 0x7E) {
            uint16_t proto;
            std::vector<uint8_t> payload;
            if (ppp_parse_frame(buf + i, total - i, &proto, &payload)) {
                if (proto == PPP_IPCP && payload.size() >= 1 && payload[0] == PPP_CONF_REQ) {
                    auto ack = ppp_build_conf_ack(payload[1], payload.data() + 4, payload.size() - 4);
                    auto ackFrame = ppp_build_frame(PPP_IPCP, ack.data(), ack.size());
                    ::write(mdm->slave_fd, ackFrame.data(), ackFrame.size());
                }
            }
        }
    }
    usleep(200000);

    // Now send Conf-Req with the assigned IP (10.64.64.2)
    uint8_t ipcpReq2[] = { PPP_CONF_REQ, 0x02, 0x00, 0x0A,
                           IPCP_OPT_IP_ADDR, 6, 10, 64, 64, 2 };
    auto ipcpFrame2 = ppp_build_frame(PPP_IPCP, ipcpReq2, sizeof(ipcpReq2));
    ::write(mdm->slave_fd, ipcpFrame2.data(), ipcpFrame2.size());
    usleep(300000);

    // Read response — should be IPCP Conf-Ack
    total = 0;
    for (int i = 0; i < 10; i++) {
        struct pollfd pfd = { mdm->slave_fd, POLLIN, 0 };
        if (poll(&pfd, 1, 100) > 0) {
            int n = ::read(mdm->slave_fd, buf + total, sizeof(buf) - total);
            if (n > 0) total += n;
        }
    }

    bool gotIpcpAck = false;
    for (int i = 0; i < total; i++) {
        if (buf[i] == 0x7E) {
            uint16_t proto;
            std::vector<uint8_t> payload;
            if (ppp_parse_frame(buf + i, total - i, &proto, &payload)) {
                if (proto == PPP_IPCP && payload.size() >= 1 && payload[0] == PPP_CONF_ACK) {
                    gotIpcpAck = true;
                }
            }
        }
    }
    TEST_ASSERT_TRUE(gotIpcpAck);

    // Verify PPP link is fully up
    TEST_ASSERT_TRUE(mdm->pppUp);

    teardownModem();
}

// ── Test runner ─────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    UNITY_BEGIN();

    // FCS
    RUN_TEST(test_fcs_deterministic);
    RUN_TEST(test_fcs_different_data);

    // Frame build/parse
    RUN_TEST(test_frame_roundtrip_lcp);
    RUN_TEST(test_frame_roundtrip_ipcp);
    RUN_TEST(test_frame_roundtrip_ip);
    RUN_TEST(test_frame_bad_fcs);
    RUN_TEST(test_frame_byte_stuffing);

    // URC parser
    RUN_TEST(test_urc_csq);
    RUN_TEST(test_urc_creg_home);
    RUN_TEST(test_urc_creg_lost);
    RUN_TEST(test_urc_between_ppp_frames);
    RUN_TEST(test_urc_ignores_binary);

    // PPP negotiation via SimModem
    RUN_TEST(test_ppp_lcp_negotiation);
    RUN_TEST(test_ppp_ipcp_assigns_ip);
    RUN_TEST(test_ppp_echo_reply);
    RUN_TEST(test_ppp_full_negotiation);

    return UNITY_END();
}

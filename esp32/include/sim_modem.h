#pragma once
// SIM7600 Modem Simulator — AT commands + PPP bridge via pppd
// After ATD*99#, starts pppd as PPP server on a second PTY and bridges
// bytes between the firmware PTY and pppd PTY with baud rate throttling.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include <sys/wait.h>

#if !defined(ESP_PLATFORM)
#include <pty.h>
#endif

#include "ppp_proto.h"

class SimModem {
public:
    // Configurable state
    int         rssi = 22;
    int         ber = 99;
    int         regStat = 1;       // +CREG stat: 1=home, 5=roaming
    const char* operatorName = "SimOperator";
    bool        echoEnabled = true;
    int         baudRate = 921600;  // effective baud for throttling

    // PTY file descriptors (firmware side)
    int master_fd = -1;   // simulator reads/writes this
    int slave_fd = -1;    // firmware reads/writes this

    // PPP state
    std::atomic<bool> pppUp{false};
    char slavePath[64] = "";  // PTY slave path for firmware-side pppd

    SimModem() {}
    ~SimModem() { stop(); }

    bool start() {
#if !defined(ESP_PLATFORM)
        char sname[64] = "";
        if (openpty(&master_fd, &slave_fd, sname, nullptr, nullptr) != 0) {
            perror("openpty");
            return false;
        }
        strlcpy(slavePath, sname, sizeof(slavePath));

        // Set raw mode on PTY (disable echo, canonical, signal processing)
        struct termios t;
        tcgetattr(slave_fd, &t);
        cfmakeraw(&t);
        tcsetattr(slave_fd, TCSANOW, &t);

        int flags = fcntl(master_fd, F_GETFL);
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

        running_ = true;
        thread_ = std::thread(&SimModem::run, this);
        printf("[SimModem] Started — slave=%s\n", slavePath);
        return true;
#else
        return false;
#endif
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        if (master_fd >= 0) { close(master_fd); master_fd = -1; }
        if (slave_fd >= 0) { close(slave_fd); slave_fd = -1; }
    }

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool dataMode_ = false;

    // PPP state
    bool lcpUp_ = false;
    bool ipcpUp_ = false;
    uint8_t lcpId_ = 1;
    uint8_t ipcpId_ = 1;
    uint32_t ourMagic_ = 0x12345678;

    // PPP frame accumulator
    std::vector<uint8_t> pppBuf_;
    bool inFrame_ = false;

    // AT command buffer
    std::string cmdBuf_;

    // +++ escape detection
    int plusCount_ = 0;
    uint64_t lastPlusMs_ = 0;
    bool gotEscapePlus_ = false;

    uint64_t now_ms() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
    }

    void respond(const char* text) {
        std::string resp = std::string("\r\n") + text + "\r\n";
        ::write(master_fd, resp.c_str(), resp.size());
    }

    // (pppd bridge removed — PPP is now handled internally)

    // ── +++ escape detection ────────────────────────────────────────────

    void checkEscape(const uint8_t* buf, int len) {
        for (int i = 0; i < len; i++) {
            if (buf[i] == '+') {
                if (plusCount_ == 0) lastPlusMs_ = now_ms();
                plusCount_++;
                if (plusCount_ >= 3) gotEscapePlus_ = true;
            } else {
                plusCount_ = 0;
                gotEscapePlus_ = false;
            }
        }
    }

    // ── AT command processing ───────────────────────────────────────────

    void processCommand(const std::string& cmd) {
        std::string c = cmd;
        while (!c.empty() && (c.back() == '\r' || c.back() == '\n')) c.pop_back();
        if (c.empty()) return;

        if (echoEnabled) {
            std::string echo = c + "\r";
            ::write(master_fd, echo.c_str(), echo.size());
        }

        std::string upper = c;
        for (auto& ch : upper) ch = toupper(ch);

        if (upper == "AT" || upper == "ATH") {
            respond("OK");
        } else if (upper == "ATE0") {
            echoEnabled = false;
            respond("OK");
        } else if (upper == "ATE1") {
            echoEnabled = true;
            respond("OK");
        } else if (upper.find("AT+CFUN=") == 0) {
            respond("OK");
        } else if (upper == "AT+CSQ") {
            char buf[32];
            snprintf(buf, sizeof(buf), "+CSQ: %d,%d", rssi, ber);
            respond(buf);
            respond("OK");
        } else if (upper == "AT+COPS?") {
            char buf[64];
            snprintf(buf, sizeof(buf), "+COPS: 0,0,\"%s\",7", operatorName);
            respond(buf);
            respond("OK");
        } else if (upper == "AT+CREG?") {
            char buf[32];
            snprintf(buf, sizeof(buf), "+CREG: 1,%d", regStat);
            respond(buf);
            respond("OK");
        } else if (upper.find("AT+CREG=") == 0) {
            respond("OK");
        } else if (upper.find("AT+AUTOCSQ=") == 0) {
            respond("OK");
        } else if (upper.find("AT+CTZU=") == 0) {
            respond("OK");
        } else if (upper == "AT+CCLK?") {
            time_t t = time(nullptr);
            struct tm tm;
            gmtime_r(&t, &tm);
            char buf[48];
            snprintf(buf, sizeof(buf), "+CCLK: \"%02d/%02d/%02d,%02d:%02d:%02d+00\"",
                     tm.tm_year % 100, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec);
            respond(buf);
            respond("OK");
        } else if (upper.find("AT+CGDCONT=") == 0) {
            respond("OK");
        } else if (upper.find("AT+IPR=") == 0) {
            respond("OK");
        } else if (upper.find("AT+IFC=") == 0) {
            respond("OK");
        } else if (upper == "ATD*99#") {
            respond("CONNECT 921600");
            dataMode_ = true;
        } else if (upper == "ATO") {
            respond("CONNECT");
            dataMode_ = true;
        } else if (upper.find("AT") == 0) {
            respond("OK");
        }
    }

    // ── PPP frame handler ─────────────────────────────────────────────

    void sendPppFrame(uint16_t protocol, const std::vector<uint8_t>& payload) {
        auto frame = ppp_build_frame(protocol, payload.data(), payload.size());
        int w = ::write(master_fd, frame.data(), frame.size());
        printf("[SimModem] Sent PPP frame proto=0x%04X (%zu bytes, wrote=%d)\n",
               protocol, frame.size(), w);
    }

    void handlePppFrame(const uint8_t* data, size_t len) {
        uint16_t protocol;
        std::vector<uint8_t> payload;
        if (!ppp_parse_frame(data, len, &protocol, &payload)) {
            printf("[SimModem] PPP frame parse failed (len=%zu, first=%02X)\n", len, len > 0 ? data[0] : 0);
            return;
        }
        printf("[SimModem] PPP frame: proto=0x%04X payload=%zu bytes\n", protocol, payload.size());

        if (protocol == PPP_LCP) {
            handleLcp(payload);
        } else if (protocol == PPP_IPCP) {
            handleIpcp(payload);
        } else if (protocol == PPP_IP) {
            handleIpPacket(payload);
        }
    }

    void handleLcp(const std::vector<uint8_t>& payload) {
        if (payload.size() < 4) return;
        uint8_t code = payload[0];
        uint8_t id = payload[1];

        if (code == PPP_CONF_REQ) {
            // ACK whatever the peer requests
            auto ack = ppp_build_conf_ack(id, payload.data() + 4, payload.size() - 4);
            sendPppFrame(PPP_LCP, ack);

            // Send our own LCP Configure-Request (empty — no options needed)
            if (!lcpUp_) {
                auto req = ppp_build_conf_req(lcpId_++, nullptr, 0);
                sendPppFrame(PPP_LCP, req);
            }
        } else if (code == PPP_CONF_ACK) {
            lcpUp_ = true;
            if (!pppUp) { pppUp = true; printf("[SimModem] LCP up\n"); }
        } else if (code == PPP_ECHO_REQ) {
            auto reply = ppp_build_echo_reply(id, ourMagic_);
            sendPppFrame(PPP_LCP, reply);
        } else if (code == PPP_TERM_REQ) {
            // ACK termination
            std::vector<uint8_t> ack = {PPP_TERM_ACK, id, 0, 4};
            sendPppFrame(PPP_LCP, ack);
            lcpUp_ = false;
            ipcpUp_ = false;
        }
    }

    void handleIpcp(const std::vector<uint8_t>& payload) {
        if (payload.size() < 4) return;
        uint8_t code = payload[0];
        uint8_t id = payload[1];

        if (code == PPP_CONF_REQ) {
            // Peer requests IP config. NAK with our assigned IP if they request 0.0.0.0
            bool needsNak = false;
            std::vector<uint8_t> nakOpts;

            // Parse options
            size_t pos = 4;
            while (pos + 2 <= payload.size()) {
                uint8_t optType = payload[pos];
                uint8_t optLen = payload[pos + 1];
                if (optLen < 2 || pos + optLen > payload.size()) break;

                if (optType == IPCP_OPT_IP_ADDR && optLen == 6) {
                    uint32_t reqIp = (payload[pos+2] << 24) | (payload[pos+3] << 16) |
                                     (payload[pos+4] << 8) | payload[pos+5];
                    if (reqIp == 0) {
                        // NAK with our assigned IP: 10.64.64.2
                        needsNak = true;
                        nakOpts.push_back(IPCP_OPT_IP_ADDR);
                        nakOpts.push_back(6);
                        nakOpts.push_back(10); nakOpts.push_back(64);
                        nakOpts.push_back(64); nakOpts.push_back(2);
                    }
                } else if ((optType == IPCP_OPT_DNS1 || optType == IPCP_OPT_DNS2) && optLen == 6) {
                    // NAK with real DNS: 8.8.8.8
                    needsNak = true;
                    nakOpts.push_back(optType);
                    nakOpts.push_back(6);
                    nakOpts.push_back(8); nakOpts.push_back(8);
                    nakOpts.push_back(8); nakOpts.push_back(8);
                }
                pos += optLen;
            }

            if (needsNak) {
                std::vector<uint8_t> nak;
                nak.push_back(PPP_CONF_NAK);
                nak.push_back(id);
                uint16_t len = 4 + nakOpts.size();
                nak.push_back((len >> 8) & 0xFF);
                nak.push_back(len & 0xFF);
                nak.insert(nak.end(), nakOpts.begin(), nakOpts.end());
                sendPppFrame(PPP_IPCP, nak);
            } else {
                // ACK — peer has correct IP
                auto ack = ppp_build_conf_ack(id, payload.data() + 4, payload.size() - 4);
                sendPppFrame(PPP_IPCP, ack);
            }

            // Send our own IPCP Configure-Request if not already up
            if (!ipcpUp_) {
                // Our IP: 10.64.64.1
                uint8_t opts[] = { IPCP_OPT_IP_ADDR, 6, 10, 64, 64, 1 };
                auto req = ppp_build_conf_req(ipcpId_++, opts, sizeof(opts));
                sendPppFrame(PPP_IPCP, req);
            }
        } else if (code == PPP_CONF_ACK) {
            ipcpUp_ = true;
            printf("[SimModem] IPCP up — PPP link established\n");
        } else if (code == PPP_CONF_NAK) {
            // Peer NAKed our config — update and retry
            // (usually means peer wants different IP options)
            ipcpUp_ = false;
        }
    }

    void handleIpPacket(const std::vector<uint8_t>& payload) {
        // IP data received from peer. In Step 2, this goes to a TUN device.
        // For now, just count bytes.
        (void)payload;
    }

    // ── Main loop ───────────────────────────────────────────────────────

    void run() {
        while (running_) {
            if (dataMode_) {
                // PPP data mode: parse frames, negotiate LCP/IPCP, watch for +++
                uint8_t buf[1024];
                struct pollfd pfd = { master_fd, POLLIN, 0 };
                int ret = poll(&pfd, 1, 50);
                if (ret > 0) {
                    int n = ::read(master_fd, buf, sizeof(buf));
                    if (n > 0) {
                        checkEscape(buf, n);
                        // Accumulate and process PPP frames
                        for (int i = 0; i < n; i++) {
                            if (buf[i] == 0x7E) {
                                if (inFrame_ && pppBuf_.size() > 0) {
                                    pppBuf_.insert(pppBuf_.begin(), 0x7E);
                                    pppBuf_.push_back(0x7E);
                                    handlePppFrame(pppBuf_.data(), pppBuf_.size());
                                }
                                pppBuf_.clear();
                                inFrame_ = true;
                            } else if (inFrame_) {
                                pppBuf_.push_back(buf[i]);
                            }
                        }
                    }
                }
                if (gotEscapePlus_ && (now_ms() - lastPlusMs_) > 500) {
                    dataMode_ = false;
                    lcpUp_ = false;
                    ipcpUp_ = false;
                    gotEscapePlus_ = false;
                    plusCount_ = 0;
                    respond("OK");
                }
            } else {
                // AT command mode
                struct pollfd pfd = { master_fd, POLLIN, 0 };
                int ret = poll(&pfd, 1, 50);
                if (ret <= 0) {
                    if (dataMode_ && gotEscapePlus_ && (now_ms() - lastPlusMs_) > 500) {
                        dataMode_ = false;
                        gotEscapePlus_ = false;
                        plusCount_ = 0;
                        respond("OK");
                    }
                    continue;
                }

                uint8_t buf[512];
                int n = ::read(master_fd, buf, sizeof(buf));
                if (n <= 0) continue;

                for (int i = 0; i < n; i++) {
                    char c = buf[i];
                    if (c == '\r' || c == '\n') {
                        if (!cmdBuf_.empty()) {
                            processCommand(cmdBuf_);
                            cmdBuf_.clear();
                        }
                    } else {
                        cmdBuf_ += c;
                    }
                }
            }
        }
    }
};

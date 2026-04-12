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
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

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
    const char* operatorName = "SimCellular";
    bool        echoEnabled = true;
    bool        apnSet = false;    // tracks AT+CGDCONT state (cleared by AT+CFUN=0, set by AT+CGDCONT)
    int         baudRate = 115200;  // tracks actual baud (persisted via AT+IPR, like real SIM7600 NVS)

    // PTY file descriptors (firmware side)
    int master_fd = -1;   // simulator reads/writes this
    int slave_fd = -1;    // firmware reads/writes this

    // PPP state
    std::atomic<bool> pppUp{false};
    char slavePath[64] = "";  // PTY slave path for firmware-side pppd

    const char* nvsPath = nullptr;  // optional file to persist modem state (baud rate)

    SimModem() {}
    SimModem(const char* persistPath) : nvsPath(persistPath) { loadState(); }
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
        closeTun();
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

    // TUN device for IP routing
    int tun_fd_ = -1;
    char tunName_[IFNAMSIZ] = "";

    bool openTun() {
        tun_fd_ = open("/dev/net/tun", O_RDWR | O_NONBLOCK);
        if (tun_fd_ < 0) { perror("[SimModem] open /dev/net/tun"); return false; }

        struct ifreq ifr = {};
        ifr.ifr_flags = IFF_TUN | IFF_NO_PI;  // TUN device, no packet info header
        if (ioctl(tun_fd_, TUNSETIFF, &ifr) < 0) {
            perror("[SimModem] TUNSETIFF");
            close(tun_fd_); tun_fd_ = -1;
            return false;
        }
        strlcpy(tunName_, ifr.ifr_name, sizeof(tunName_));

        // Configure the TUN interface: assign IP, bring up, add route
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "sudo ip addr add 10.64.64.1/24 dev %s 2>/dev/null", tunName_);
        system(cmd);
        snprintf(cmd, sizeof(cmd), "sudo ip link set %s up", tunName_);
        system(cmd);
        // NAT: masquerade traffic from TUN to the internet
        system("sudo iptables -t nat -C POSTROUTING -s 10.64.64.0/24 -j MASQUERADE 2>/dev/null || "
               "sudo iptables -t nat -A POSTROUTING -s 10.64.64.0/24 -j MASQUERADE");

        printf("[SimModem] TUN device %s created (10.64.64.1/24)\n", tunName_);
        return true;
    }

    void closeTun() {
        if (tun_fd_ >= 0) { close(tun_fd_); tun_fd_ = -1; }
    }

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

    // ── Modem NVS persistence (baud rate survives restarts) ────────────

    void loadState() {
        if (!nvsPath) return;
        FILE* f = fopen(nvsPath, "r");
        if (!f) return;
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            int b;
            if (sscanf(line, "baud=%d", &b) == 1 && b > 0) baudRate = b;
        }
        fclose(f);
        printf("[SimModem] Loaded state: baud=%d\n", baudRate);
    }

    void saveState() {
        if (!nvsPath) return;
        FILE* f = fopen(nvsPath, "w");
        if (!f) return;
        fprintf(f, "baud=%d\n", baudRate);
        fclose(f);
    }

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
            int func = atoi(c.c_str() + 8);
            if (func == 0) apnSet = false;  // radio off clears PDP context (like real SIM7600)
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
            apnSet = true;
            respond("OK");
        } else if (upper.find("AT+IPR=") == 0) {
            int newBaud = atoi(c.c_str() + 7);
            if (newBaud > 0) {
                baudRate = newBaud;
                saveState();  // persist like real SIM7600 NVS
                printf("[SimModem] Baud rate set to %d (saved)\n", baudRate);
            }
            respond("OK");
        } else if (upper.find("AT+IFC=") == 0) {
            respond("OK");
        } else if (upper == "ATD*99#") {
            if (!apnSet) {
                respond("NO CARRIER");  // PDP context not set — like real SIM7600
            } else {
                respond("CONNECT 921600");
                dataMode_ = true;
            }
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
            // Open TUN device for IP routing
            if (tun_fd_ < 0) openTun();
        } else if (code == PPP_CONF_NAK) {
            // Peer NAKed our config — update and retry
            // (usually means peer wants different IP options)
            ipcpUp_ = false;
        }
    }

    void handleIpPacket(const std::vector<uint8_t>& payload) {
        // Write IP packet to TUN device for routing to the internet
        if (tun_fd_ >= 0 && payload.size() > 0) {
            ::write(tun_fd_, payload.data(), payload.size());
        }
    }

    // ── Main loop ───────────────────────────────────────────────────────

    void run() {
        while (running_) {
            if (dataMode_) {
                // PPP data mode: parse frames, negotiate LCP/IPCP, watch for +++
                // Throttle: limit bytes per tick to match baud rate
                // 8N1 = 10 bits per byte, so max bytes/sec = baudRate / 10
                int maxBytesPerTick = baudRate / 10 / 100;  // per 10ms tick
                if (maxBytesPerTick < 16) maxBytesPerTick = 16;

                uint8_t buf[1024];
                int readLimit = (maxBytesPerTick < (int)sizeof(buf)) ? maxBytesPerTick : sizeof(buf);

                struct pollfd pfd = { master_fd, POLLIN, 0 };
                int ret = poll(&pfd, 1, 10);  // 10ms tick
                int bytesThisTick = 0;

                if (ret > 0) {
                    int n = ::read(master_fd, buf, readLimit);
                    if (n > 0) {
                        bytesThisTick += n;
                        checkEscape(buf, n);
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

                // Read IP packets from TUN → wrap in PPP → send to firmware
                if (tun_fd_ >= 0 && ipcpUp_ && bytesThisTick < maxBytesPerTick) {
                    uint8_t tunBuf[1500];
                    int tn = ::read(tun_fd_, tunBuf, sizeof(tunBuf));
                    if (tn > 0) {
                        auto frame = ppp_build_frame(PPP_IP, tunBuf, tn);
                        // Throttle: limit how fast we send to firmware
                        int allowed = maxBytesPerTick - bytesThisTick;
                        if ((int)frame.size() <= allowed) {
                            ::write(master_fd, frame.data(), frame.size());
                            bytesThisTick += frame.size();
                        }
                        // else: drop this tick, will retry next tick
                    }
                }

                // Sleep to enforce tick rate — this is the actual throttle
                if (bytesThisTick > 0) {
                    // Sleep proportional to bytes transferred at baud rate
                    // time_us = bytes * 10 bits * 1e6 / baudRate
                    uint32_t sleep_us = (uint32_t)bytesThisTick * 10000000ULL / baudRate;
                    if (sleep_us > 0) usleep(sleep_us);
                }

                if (gotEscapePlus_ && (now_ms() - lastPlusMs_) > 500) {
                    dataMode_ = false;
                    lcpUp_ = false;
                    ipcpUp_ = false;
                    closeTun();
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

#pragma once
// SIM7600 Modem Simulator — AT commands + PPP bridge via pppd
// After ATD*99#, starts pppd as PPP server on a second PTY and bridges
// bytes between the firmware PTY and pppd PTY with baud rate throttling.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>

#if !defined(ESP_PLATFORM)
#include <pty.h>
#endif

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
        stopPppd();
        if (thread_.joinable()) thread_.join();
        if (master_fd >= 0) { close(master_fd); master_fd = -1; }
        if (slave_fd >= 0) { close(slave_fd); slave_fd = -1; }
    }

private:
    std::thread thread_;
    std::atomic<bool> running_{false};
    bool dataMode_ = false;

    // PPP bridge
    int pppd_master_fd_ = -1;  // our side of the pppd PTY
    int pppd_slave_fd_ = -1;   // pppd's side
    pid_t pppd_pid_ = -1;

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

    // ── PPP bridge management ───────────────────────────────────────────

    bool startPppd() {
#if !defined(ESP_PLATFORM)
        char pppSlaveName[64] = "";
        if (openpty(&pppd_master_fd_, &pppd_slave_fd_, pppSlaveName, nullptr, nullptr) != 0) {
            perror("[SimModem] openpty for pppd");
            return false;
        }

        pppd_pid_ = fork();
        if (pppd_pid_ < 0) {
            perror("[SimModem] fork");
            close(pppd_master_fd_); close(pppd_slave_fd_);
            pppd_master_fd_ = pppd_slave_fd_ = -1;
            return false;
        }

        if (pppd_pid_ == 0) {
            // Child: exec pppd as PPP server
            close(pppd_master_fd_);
            close(master_fd);
            close(slave_fd);
            // Redirect stdout/stderr so pppd doesn't pollute emulator output
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }

            execlp("sudo", "sudo", "pppd",
                pppSlaveName,
                "921600",
                "noauth",
                "nodetach",
                "passive",
                "local",
                "10.64.64.1:10.64.64.2",
                "ms-dns", "8.8.8.8",
                "proxyarp",
                "silent",
                "logfile", "/tmp/sim_pppd_server.log",
                (char*)nullptr);
            perror("execlp pppd");
            _exit(1);
        }

        // Parent: close slave (pppd uses it)
        close(pppd_slave_fd_);
        pppd_slave_fd_ = -1;

        // Make pppd master non-blocking
        int flags = fcntl(pppd_master_fd_, F_GETFL);
        fcntl(pppd_master_fd_, F_SETFL, flags | O_NONBLOCK);

        printf("[SimModem] pppd started (pid=%d) on %s\n", pppd_pid_, pppSlaveName);
        return true;
#else
        return false;
#endif
    }

    void stopPppd() {
        if (pppd_pid_ > 0) {
            kill(pppd_pid_, SIGTERM);
            int status;
            waitpid(pppd_pid_, &status, WNOHANG);
            pppd_pid_ = -1;
        }
        if (pppd_master_fd_ >= 0) { close(pppd_master_fd_); pppd_master_fd_ = -1; }
        pppUp = false;
    }

    // Bridge bytes with baud rate throttling
    // Returns bytes bridged (for stats)
    int bridgeBytes() {
        if (pppd_master_fd_ < 0) return 0;

        // Throttle: max bytes per 10ms tick at configured baud
        // baud/10 = bytes/sec (8N1 = 10 bits per byte), /100 = bytes per 10ms
        int maxPerTick = baudRate / 10 / 100;
        if (maxPerTick < 64) maxPerTick = 64;

        int total = 0;
        uint8_t buf[4096];

        // Firmware → pppd
        int n = ::read(master_fd, buf, std::min((int)sizeof(buf), maxPerTick));
        if (n > 0) {
            ::write(pppd_master_fd_, buf, n);
            total += n;
        }

        // pppd → firmware
        n = ::read(pppd_master_fd_, buf, std::min((int)sizeof(buf), maxPerTick));
        if (n > 0) {
            ::write(master_fd, buf, n);
            total += n;
            if (!pppUp) {
                pppUp = true;
                printf("[SimModem] PPP data flowing\n");
            }
        }

        return total;
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
            if (pppd_master_fd_ >= 0) {
                respond("CONNECT");
                dataMode_ = true;
            } else {
                respond("NO CARRIER");
            }
        } else if (upper.find("AT") == 0) {
            respond("OK");
        }
    }

    // ── Main loop ───────────────────────────────────────────────────────

    void run() {
        while (running_) {
            if (dataMode_) {
                // PPP data mode: absorb bytes, watch for +++ escape
                uint8_t buf[512];
                struct pollfd pfd = { master_fd, POLLIN, 0 };
                int ret = poll(&pfd, 1, 50);
                if (ret > 0) {
                    int n = ::read(master_fd, buf, sizeof(buf));
                    if (n > 0) checkEscape(buf, n);
                }
                if (gotEscapePlus_ && (now_ms() - lastPlusMs_) > 500) {
                    dataMode_ = false;
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
        stopPppd();
    }
};

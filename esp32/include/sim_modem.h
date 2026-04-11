#pragma once
// SIM7600 Modem Simulator — responds to AT commands over a PTY
// Runs in a background thread. The firmware side reads/writes the slave fd.

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

#if !defined(ESP_PLATFORM)
#include <pty.h>
#endif

class SimModem {
public:
    // Configurable state
    int         rssi = 22;         // CSQ signal quality (0-31, 99=unknown)
    int         ber = 99;          // bit error rate
    int         regStat = 1;       // +CREG: stat (1=home, 5=roaming)
    const char* operatorName = "SimOperator";
    const char* apn = "hologram";
    bool        echoEnabled = true;

    // PTY file descriptors
    int master_fd = -1;  // simulator reads/writes this
    int slave_fd = -1;   // firmware reads/writes this

    SimModem() {}
    ~SimModem() { stop(); }

    bool start() {
#if !defined(ESP_PLATFORM)
        if (openpty(&master_fd, &slave_fd, nullptr, nullptr, nullptr) != 0) {
            perror("openpty");
            return false;
        }
        // Make master non-blocking for poll
        int flags = fcntl(master_fd, F_GETFL);
        fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

        running_ = true;
        thread_ = std::thread(&SimModem::run, this);
        printf("[SimModem] Started on fd=%d (slave=%d)\n", master_fd, slave_fd);
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
    bool dataMode_ = false;  // true = PPP data mode, false = AT command mode

    std::string cmdBuf_;
    bool gotEscapePlus_ = false;
    int plusCount_ = 0;
    uint64_t lastPlusMs_ = 0;

    uint64_t now_ms() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
    }

    void respond(const char* text) {
        // Send response with \r\n framing
        std::string resp = std::string("\r\n") + text + "\r\n";
        ::write(master_fd, resp.c_str(), resp.size());
    }

    void processCommand(const std::string& cmd) {
        // Strip leading whitespace/AT prefix variations
        std::string c = cmd;
        // Remove trailing \r\n
        while (!c.empty() && (c.back() == '\r' || c.back() == '\n')) c.pop_back();
        if (c.empty()) return;

        // Echo if enabled
        if (echoEnabled) {
            std::string echo = c + "\r";
            ::write(master_fd, echo.c_str(), echo.size());
        }

        // Uppercase for matching
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
            // Return current UTC time
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
            // Enter PPP data mode
            respond("CONNECT 115200");
            dataMode_ = true;
        } else if (upper == "ATO") {
            // Return to data mode
            respond("CONNECT");
            dataMode_ = true;
        } else if (upper.find("AT") == 0) {
            // Unknown AT command — respond OK
            respond("OK");
        }
    }

    void handleDataByte(uint8_t byte) {
        // In PPP data mode, we receive PPP frames from the firmware
        // For simulation, we just absorb them (no real PPP negotiation)
        // Periodically send URCs between PPP frames
        (void)byte;
    }

    void checkEscapeSequence(const uint8_t* buf, int len) {
        // Detect +++ escape: 3 consecutive '+' chars with silence before/after
        for (int i = 0; i < len; i++) {
            if (buf[i] == '+') {
                if (plusCount_ == 0) lastPlusMs_ = now_ms();
                plusCount_++;
                if (plusCount_ >= 3) {
                    gotEscapePlus_ = true;
                }
            } else {
                plusCount_ = 0;
                gotEscapePlus_ = false;
            }
        }
    }

    void run() {
        struct pollfd pfd = { master_fd, POLLIN, 0 };
        uint64_t lastDataMs = 0;

        while (running_) {
            int ret = poll(&pfd, 1, 50);  // 50ms poll timeout
            if (ret <= 0) {
                // Check for +++ escape completion (silence after +++)
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

            if (dataMode_) {
                checkEscapeSequence(buf, n);
                for (int i = 0; i < n; i++) handleDataByte(buf[i]);
                lastDataMs = now_ms();
            } else {
                // Command mode: accumulate bytes into command buffer
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

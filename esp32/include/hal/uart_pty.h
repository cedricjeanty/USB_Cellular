#pragma once
// PTY-based UART implementation — connects to SimModem via PTY slave fd
// For use in emulator and native tests.

#include "hal/uart.h"
#include <unistd.h>
#include <poll.h>
#include <cstdint>
#include <cstring>

class PtyUart : public IUart {
public:
    int fd = -1;  // slave end of PTY

    PtyUart() {}
    PtyUart(int slave_fd) : fd(slave_fd) {}

    int write(const void* data, size_t len) override {
        if (fd < 0) return -1;
        return ::write(fd, data, len);
    }

    int read(void* buf, size_t len, uint32_t timeout_ms) override {
        if (fd < 0) return -1;
        struct pollfd pfd = { fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return 0;  // timeout or error
        int n = ::read(fd, buf, len);
        return (n > 0) ? n : 0;
    }

    void flush() override {
        if (fd < 0) return;
        // Drain any pending input
        uint8_t tmp[256];
        struct pollfd pfd = { fd, POLLIN, 0 };
        while (poll(&pfd, 1, 0) > 0) {
            if (::read(fd, tmp, sizeof(tmp)) <= 0) break;
        }
    }

    void set_baudrate(uint32_t baud) override {
        // PTY doesn't have real baud — just record it
        (void)baud;
    }

    void set_flow_control(bool enable) override {
        (void)enable;
    }
};

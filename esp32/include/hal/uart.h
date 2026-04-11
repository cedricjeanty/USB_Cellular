#pragma once
// AirBridge UART HAL — serial port abstraction
// ESP32: wraps uart_driver. Native: PTY file descriptor.

#include <cstdint>
#include <cstddef>

class IUart {
public:
    virtual ~IUart() = default;

    // Write bytes to the UART. Returns bytes written.
    virtual int write(const void* data, size_t len) = 0;

    // Read bytes from the UART with timeout in ms. Returns bytes read.
    virtual int read(void* buf, size_t len, uint32_t timeout_ms) = 0;

    // Flush RX buffer
    virtual void flush() = 0;

    // Set baud rate
    virtual void set_baudrate(uint32_t baud) = 0;

    // Enable/disable hardware flow control
    virtual void set_flow_control(bool enable) = 0;
};

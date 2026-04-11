#pragma once
// AirBridge Network HAL — TLS connection abstraction
// ESP32: wraps esp_tls. Native: canned HTTP response queue.

#include <cstdint>
#include <cstddef>
#include <string>

// Opaque TLS connection handle
typedef void* TlsHandle;

class INetwork {
public:
    virtual ~INetwork() = default;

    // Connect to host:443 via TLS. Returns handle or nullptr.
    virtual TlsHandle connect(const char* host) = 0;

    // Write all bytes. Returns true on success.
    virtual bool write(TlsHandle conn, const void* data, size_t len) = 0;

    // Read up to len bytes. Returns bytes read, 0 on close, <0 on error.
    virtual int read(TlsHandle conn, void* buf, size_t len) = 0;

    // Close and destroy the connection.
    virtual void destroy(TlsHandle conn) = 0;
};

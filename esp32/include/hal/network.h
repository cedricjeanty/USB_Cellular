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

    virtual TlsHandle connect(const char* host) = 0;
    virtual bool write(TlsHandle conn, const void* data, size_t len) = 0;
    virtual int read(TlsHandle conn, void* buf, size_t len) = 0;
    virtual void destroy(TlsHandle conn) = 0;

    // Bandwidth limit in bytes/sec (0 = unlimited). Used by halStreamFile for throttling.
    virtual int getMaxBytesPerSec() { return 0; }
};

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
    // Write ALL bytes (until done or error). Real-TLS impls route this through the shared
    // netWriteAll() (hal/net_util.h), which loops writeSome() with a wall-clock no-progress
    // timeout so a dead link fails instead of spinning forever (and wedging the upload task).
    virtual bool write(TlsHandle conn, const void* data, size_t len) = 0;
    // Non-blocking write primitive used by netWriteAll(): returns >0 bytes written,
    // 0 = would-block (retry later), <0 = error. (Mocks may just return len.)
    virtual int writeSome(TlsHandle conn, const void* data, size_t len) = 0;
    virtual int read(TlsHandle conn, void* buf, size_t len) = 0;
    virtual void destroy(TlsHandle conn) = 0;

    // Bandwidth limit in bytes/sec (0 = unlimited). Used by halStreamFile for throttling.
    virtual int getMaxBytesPerSec() { return 0; }
};

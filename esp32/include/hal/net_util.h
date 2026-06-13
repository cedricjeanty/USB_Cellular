#pragma once
// Shared network write helper.
//
// Write ALL bytes via the network's non-blocking writeSome() primitive, bounded by a
// WALL-CLOCK no-progress timeout. On a dead/half-dead cellular link a non-blocking TLS
// socket keeps returning "would-block" (0) forever — without this bound the write loop
// spins indefinitely and wedges the upload task, so the device stops reconnecting (a real
// in-field failure). Returning false on a stall lets the caller fail the upload and let the
// modem reconnect logic run. Uses g_hal->clock for time + backoff, so it's deterministic
// and unit-testable (advance a test clock to trip the timeout).
//
// This is the single home for that timeout — both real-TLS impls (Esp32Network, OpenSSL
// Network) route their write() through it, instead of each hand-rolling the loop.

#include "hal/hal.h"

inline bool netWriteAll(INetwork* net, TlsHandle conn, const void* data, size_t len,
                        uint32_t noProgressMs = 30000) {
    const char* p = (const char*)data;
    size_t rem = len;
    uint32_t lastProgress = g_hal->clock->millis();
    while (rem > 0) {
        int w = net->writeSome(conn, p, rem);
        if (w > 0) {
            p += (size_t)w;
            rem -= (size_t)w;
            lastProgress = g_hal->clock->millis();
        } else if (w == 0) {                       // would-block
            if (g_hal->clock->millis() - lastProgress > noProgressMs) return false;
            g_hal->clock->delay_ms(5);
        } else {                                   // error
            return false;
        }
    }
    return true;
}

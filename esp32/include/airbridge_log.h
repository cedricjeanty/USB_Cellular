#pragma once
// AirBridge unified logging — single function writes to serial + RAM buffer.
// Serial sink: CDC (firmware) or PTY+stdout (emulator).
// Buffer: flushed to SD file periodically, uploaded to S3.
//
// Usage:
//   airbridge_log_init(serial_fn, uptime_fn);  // once at boot
//   airbridge_log("Modem: RSSI=%d", rssi);     // from any task/thread
//   airbridge_log_flush("/sdcard/airbridge.log"); // during harvest (exclusive SD)

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <cstdint>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <pthread.h>
#endif

#define AIRBRIDGE_LOG_BUF_SIZE 8192

// ── Platform mutex abstraction ──────────────────────────────────────────────

#ifdef ESP_PLATFORM
static SemaphoreHandle_t s_log_mutex = nullptr;
static inline void _log_mutex_init() { s_log_mutex = xSemaphoreCreateMutex(); }
static inline bool _log_mutex_take() {
    return s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE;
}
static inline void _log_mutex_give() { if (s_log_mutex) xSemaphoreGive(s_log_mutex); }
#else
static pthread_mutex_t s_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static inline void _log_mutex_init() { pthread_mutex_init(&s_log_mutex, nullptr); }
static inline bool _log_mutex_take() { return pthread_mutex_lock(&s_log_mutex) == 0; }
static inline void _log_mutex_give() { pthread_mutex_unlock(&s_log_mutex); }
#endif

// ── State ───────────────────────────────────────────────────────────────────

static char         s_logbuf[AIRBRIDGE_LOG_BUF_SIZE];
static int          s_loglen = 0;
static void       (*s_serial_fn)(const char* buf, int len) = nullptr;
static uint32_t   (*s_uptime_fn)() = nullptr;
static uint32_t     s_boot_epoch = 0;   // Unix timestamp at boot (0 = not synced)
static uint32_t     s_boot_ms = 0;      // millis() when epoch was captured

// ── API ─────────────────────────────────────────────────────────────────────

inline void airbridge_log_init(void (*serial_fn)(const char*, int),
                                uint32_t (*uptime_fn)()) {
    _log_mutex_init();
    s_serial_fn = serial_fn;
    s_uptime_fn = uptime_fn;
    s_loglen = 0;
}

inline void airbridge_log_set_time(uint32_t epoch, uint32_t captureMs) {
    s_boot_epoch = epoch;
    s_boot_ms = captureMs;
}

inline void airbridge_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
inline void airbridge_log(const char* fmt, ...) {
    // Format message into stack buffer
    char line[300];  // timestamp(~20) + message(~256) + newline
    int pos = 0;

    // Timestamp
    uint32_t now_ms = s_uptime_fn ? s_uptime_fn() : 0;
    if (s_boot_epoch > 0) {
        uint32_t now = s_boot_epoch + (now_ms - s_boot_ms) / 1000;
        time_t t = (time_t)now;
        struct tm tm;
        gmtime_r(&t, &tm);
        pos = snprintf(line, sizeof(line), "[%02d/%02d %02d:%02d:%02d] ",
                       tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        pos = snprintf(line, sizeof(line), "[+%lus] ", (unsigned long)(now_ms / 1000));
    }

    // Message
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line + pos, sizeof(line) - pos - 1, fmt, args);
    va_end(args);
    if (n > 0) pos += (n < (int)sizeof(line) - pos - 1) ? n : (int)sizeof(line) - pos - 2;

    // Newline
    line[pos++] = '\n';
    line[pos] = '\0';

    // Write to serial sink (outside mutex — CDC/PTY writes are safe)
    if (s_serial_fn) {
        s_serial_fn(line, pos);
    }

    // Append to ring buffer (under mutex)
    if (_log_mutex_take()) {
        int avail = AIRBRIDGE_LOG_BUF_SIZE - s_loglen;
        if (pos <= avail) {
            memcpy(s_logbuf + s_loglen, line, pos);
            s_loglen += pos;
        }
        // If buffer getting full, discard oldest half
        if (s_loglen > AIRBRIDGE_LOG_BUF_SIZE - 256) {
            int half = s_loglen / 2;
            while (half < s_loglen && s_logbuf[half] != '\n') half++;
            if (half < s_loglen) half++;
            memmove(s_logbuf, s_logbuf + half, s_loglen - half);
            s_loglen -= half;
        }
        _log_mutex_give();
    }
}

// Snapshot the log buffer for S3 upload (caller provides buffer)
inline int airbridge_log_snapshot(char* out, int maxlen) {
    if (!_log_mutex_take()) return 0;
    int n = (s_loglen < maxlen) ? s_loglen : maxlen;
    memcpy(out, s_logbuf, n);
    _log_mutex_give();
    return n;
}

// Flush log buffer to a file (call with exclusive filesystem access)
inline void airbridge_log_flush(const char* path) {
    if (s_loglen == 0) return;
    if (!_log_mutex_take()) return;

    FILE* f = fopen(path, "a");
    if (f) {
        fseek(f, 0, SEEK_END);
        if (ftell(f) > 64 * 1024) {
            fclose(f);
            f = fopen(path, "w");  // truncate if too large
        }
        if (f) {
            fwrite(s_logbuf, 1, s_loglen, f);
            fclose(f);
        }
    }
    s_loglen = 0;
    _log_mutex_give();
}

// Clear the buffer after S3 upload
inline void airbridge_log_clear() {
    if (_log_mutex_take()) {
        s_loglen = 0;
        _log_mutex_give();
    }
}

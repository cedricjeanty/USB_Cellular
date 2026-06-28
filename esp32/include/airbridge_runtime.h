#pragma once
// AirBridge runtime utilities — OTA check, speed calculation, log buffer,
// STATUS formatting. All hardware-independent, usable in firmware + emulator.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_http.h"
#include "airbridge_s3.h"
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <string>

// ── OTA version check via HAL ───────────────────────────────────────────────

struct OtaCheckResult {
    int status;            // 1=update available, 0=up to date, -1=error
    char newVersion[16];   // version string if update available
    uint32_t size;         // firmware size in bytes
    char downloadUrl[2500]; // presigned download URL
};

// ── No-progress watchdog ────────────────────────────────────────────────────
// The main loop stamps a heartbeat every iteration. An independent watchdog task
// (which takes no contended locks) reboots the device if the heartbeat goes stale
// — i.e. the main loop wedged. This catches the hard-hang class that bricked a
// field unit for 9 hours (a task spun holding the SD/log mutex; idle tasks kept
// feeding the ESP task-WDT, so it never tripped). Unsigned subtraction tolerates
// millis() wraparound. lastHeartbeatMs==0 means "not started yet" → never reboot.
inline bool watchdogShouldReboot(uint32_t lastHeartbeatMs, uint32_t nowMs,
                                 uint32_t stallMs) {
    if (lastHeartbeatMs == 0) return false;
    return (uint32_t)(nowMs - lastHeartbeatMs) > stallMs;
}

// ── SD-card runtime health / recovery escalation ────────────────────────────
// A card that goes bad mid-flight (corrupt FAT, SPI flake) must never silently
// wedge the unit. The firmware probes the SD periodically and feeds the count of
// CONSECUTIVE failures here to decide how far to escalate recovery. The gradient
// is deliberately conservative — non-destructive first, destructive only as a
// last resort:
//   NONE     — healthy, or too few failures to act (keep probing/retrying).
//   DEGRADE  — treat the card as unmounted: stop touching it, surface "SD ERROR"
//              on the OLED, and egress logs over cellular straight from the RAM
//              ring buffer (no SD needed) so the failure is visible remotely.
//              Remount is retried each cycle; a transient flake recovers here
//              with no data loss and never reaches REFORMAT.
//   REFORMAT — the card is persistently unusable: reformat it (losing the queued
//              data, which the DSU re-sends via the cookie) and reboot into the
//              boot-time format path. The destructive step the user explicitly
//              authorized: "if the device cannot operate because of a corrupt
//              fat, we do need to reformat and lose data."
// degradeAfter/reformatAfter are failure counts (0 disables that tier).
enum SdRecoveryAction { SD_RECOVERY_NONE = 0, SD_RECOVERY_DEGRADE, SD_RECOVERY_REFORMAT };

inline SdRecoveryAction sdRecoveryAction(uint32_t consecutiveFailures,
                                         uint32_t degradeAfter,
                                         uint32_t reformatAfter) {
    if (reformatAfter > 0 && consecutiveFailures >= reformatAfter) return SD_RECOVERY_REFORMAT;
    if (degradeAfter  > 0 && consecutiveFailures >= degradeAfter)  return SD_RECOVERY_DEGRADE;
    return SD_RECOVERY_NONE;
}

// The full health state machine: tracks consecutive probe failures and returns
// the escalation. Shared by the firmware's sd_health_check and the emulator's
// health loop so both run ONE state machine — only the platform-specific
// degrade/reformat *actions* stay in glue. Call once per probe with its result.
struct SdHealthState {
    uint32_t consecutiveFailures = 0;
};
inline SdRecoveryAction sdHealthUpdate(SdHealthState& st, bool probeOk,
                                       uint32_t degradeAfter, uint32_t reformatAfter) {
    if (probeOk) { st.consecutiveFailures = 0; return SD_RECOVERY_NONE; }
    if (st.consecutiveFailures < 0xFFFFFFFFu) st.consecutiveFailures++;
    return sdRecoveryAction(st.consecutiveFailures, degradeAfter, reformatAfter);
}

// ── Harvest-integrity guard ─────────────────────────────────────────────────
// Defense against silently losing or corrupting a flight: the harvest fires
// because the host (DSU) wrote files to the SD, but it must verify it actually
// recovered roughly what was written. If it detected a substantial write but
// harvested little or nothing — the data hasn't committed to the SD yet
// (host-cache / flush lag after a USB write), or the file was truncated/unreadable
// — the firmware must NOT silently move on. It treats the harvest as incomplete:
// logs the anomaly (so it's visible on serial + over cellular) and retries on the
// next quiet window instead of dropping the flight or uploading a stub.
// (Surfaced on hardware: device counted a 4457 KB write but harvested 0 files /
// a 28-byte truncated record.) Caller bounds retries so a genuinely empty write
// doesn't loop forever.
//   detectedWriteKB — bytes the host wrote since the last harvest (MSC/file-watch).
//   harvestedCount  — files harvestFiles actually moved.
//   harvestedKB     — bytes harvestFiles actually moved.
//   minKB           — ignore writes below this (metadata churn, tiny touches).
inline bool harvestLooksIncomplete(uint32_t detectedWriteKB, uint16_t harvestedCount,
                                   uint32_t harvestedKB, uint32_t minKB = 64) {
    if (detectedWriteKB < minKB) return false;          // nothing substantial written
    if (harvestedCount == 0) return true;               // wrote a lot, moved nothing
    if ((uint64_t)harvestedKB * 4 < detectedWriteKB) return true;  // recovered ≪ written (truncated)
    return false;
}

// Check for OTA firmware update via the S3 API.
// Does NOT download or flash — just checks version and gets URL.
inline OtaCheckResult halOtaCheck(const char* currentVersion) {
    OtaCheckResult r = {};
    if (!g_hal || !g_hal->network || !g_hal->nvs) { r.status = -1; return r; }

    S3Creds creds = loadS3Creds();
    if (!creds.valid) { r.status = 0; return r; }  // no creds = don't retry

    // GET /prod/firmware — version check
    TlsHandle tls = g_hal->network->connect(creds.apiHost);
    if (!tls) { r.status = -1; return r; }

    // device= so the backend routes the OTA version check to the correct bucket
    // (TEST_ ids -> test bucket, like presign/manifest). Without it, _pick_bucket
    // defaults to production and a test device never sees the deployed test firmware.
    char req[512];
    snprintf(req, sizeof(req),
        "GET /prod/firmware?device=%s HTTP/1.1\r\nHost: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
        creds.deviceId, creds.apiHost, creds.apiKey);
    if (!g_hal->network->write(tls, req, strlen(req))) {
        g_hal->network->destroy(tls);
        r.status = -1;
        return r;
    }
    std::string verResp = halHttpReadResponse(tls);
    g_hal->network->destroy(tls);

    if (verResp.empty() || verResp.find("\"error\"") != std::string::npos) {
        r.status = 0;  // no firmware available
        return r;
    }

    std::string newVer = jsonStr(verResp, "version");
    int fwSize = jsonInt(verResp, "size");

    if (newVer.empty() || !versionNewer(newVer.c_str(), currentVersion)) {
        r.status = 0;  // up to date
        return r;
    }

    // Update available — get download URL
    tls = g_hal->network->connect(creds.apiHost);
    if (!tls) { r.status = -1; return r; }

    snprintf(req, sizeof(req),
        "GET /prod/firmware/download?device=%s HTTP/1.1\r\nHost: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
        creds.deviceId, creds.apiHost, creds.apiKey);
    if (!g_hal->network->write(tls, req, strlen(req))) {
        g_hal->network->destroy(tls);
        r.status = -1;
        return r;
    }
    std::string dlResp = halHttpReadResponse(tls);
    g_hal->network->destroy(tls);

    std::string dlUrl = jsonStr(dlResp, "url");
    if (dlUrl.empty()) { r.status = -1; return r; }

    r.status = 1;
    strlcpy(r.newVersion, newVer.c_str(), sizeof(r.newVersion));
    r.size = (fwSize > 0) ? (uint32_t)fwSize : 0;
    strlcpy(r.downloadUrl, dlUrl.c_str(), sizeof(r.downloadUrl));
    return r;
}

// ── OTA download via HAL ─────────────────────────────────────────────────────

struct OtaDownloadResult {
    bool success;
    uint32_t bytesDownloaded;
    char error[128];
};

// Download firmware binary from the URL in OtaCheckResult.
// Saves to outputPath via HAL filesystem. Verifies size matches.
inline OtaDownloadResult halOtaDownload(const OtaCheckResult& ota, const char* outputPath,
                                         UploadProgressFn progress = nullptr) {
    OtaDownloadResult r = {};
    if (!g_hal || !g_hal->network || !g_hal->filesys) {
        strlcpy(r.error, "HAL not initialized", sizeof(r.error));
        return r;
    }
    if (ota.status != 1 || !ota.downloadUrl[0]) {
        strlcpy(r.error, "No update available", sizeof(r.error));
        return r;
    }

    // Parse download URL
    char host[128], path[2500];
    if (!parseUrl(std::string(ota.downloadUrl), host, sizeof(host), path, sizeof(path))) {
        strlcpy(r.error, "URL parse failed", sizeof(r.error));
        return r;
    }

    // Connect and send GET
    TlsHandle tls = g_hal->network->connect(host);
    if (!tls) { strlcpy(r.error, "TLS connect failed", sizeof(r.error)); return r; }

    char hdr[2700];
    snprintf(hdr, sizeof(hdr),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (!g_hal->network->write(tls, hdr, strlen(hdr))) {
        g_hal->network->destroy(tls);
        strlcpy(r.error, "Request failed", sizeof(r.error));
        return r;
    }

    // Skip HTTP headers
    char linebuf[512];
    while (true) {
        int pos = 0;
        while (pos < (int)sizeof(linebuf) - 1) {
            char c;
            int rd = g_hal->network->read(tls, &c, 1);
            if (rd <= 0) goto done;
            linebuf[pos++] = c;
            if (c == '\n') break;
        }
        linebuf[pos] = '\0';
        if (pos <= 2 && (linebuf[0] == '\r' || linebuf[0] == '\n')) break;
    }

    // Stream body to file
    {
        void* f = g_hal->filesys->open(outputPath, "wb");
        if (!f) {
            g_hal->network->destroy(tls);
            strlcpy(r.error, "Can't create output file", sizeof(r.error));
            return r;
        }

        uint8_t buf[4096];
        while (true) {
            int n = g_hal->network->read(tls, buf, sizeof(buf));
            if (n <= 0) break;
            g_hal->filesys->write(f, buf, n);
            r.bytesDownloaded += n;
            if (progress) progress(r.bytesDownloaded, ota.size);
        }
        g_hal->filesys->close(f);
    }
done:
    g_hal->network->destroy(tls);

    // Verify size
    if (ota.size > 0 && r.bytesDownloaded != ota.size) {
        snprintf(r.error, sizeof(r.error), "Size mismatch: got %u, expected %u",
                 r.bytesDownloaded, ota.size);
        return r;
    }

    r.success = true;
    return r;
}

// ── Speed calculation (EMA smoothing) ───────────────────────────────────────

// 10-second sliding window speed tracker.
// Stores (timestamp, totalMb) samples and computes average KB/s over the window.
// Gives smooth, stable speed readings suitable for display and ETA.
struct SpeedTracker {
    static const int MAX_SAMPLES = 20;  // 20 samples × 500ms = 10s window
    struct Sample { uint32_t ms; float mb; };
    Sample samples[MAX_SAMPLES];
    int head = 0;
    int count = 0;
    float lastSpeed = 0;

    // Record a sample and return smoothed speed in KB/s.
    float update(float currentMb, uint32_t nowMs) {
        // Add sample
        samples[head] = { nowMs, currentMb };
        head = (head + 1) % MAX_SAMPLES;
        if (count < MAX_SAMPLES) count++;

        // Find oldest sample within the 10s window
        int oldest = (head - count + MAX_SAMPLES) % MAX_SAMPLES;
        uint32_t dt = nowMs - samples[oldest].ms;
        float deltaMb = currentMb - samples[oldest].mb;

        if (dt < 500) return lastSpeed;  // not enough time elapsed

        if (deltaMb > 0.0001f) {
            lastSpeed = deltaMb * 1024.0f / (dt / 1000.0f);
        } else {
            lastSpeed = 0;  // no progress in window → zero
        }
        return lastSpeed;
    }
};

// ── Log buffer (mutex-free version for testing/emulator) ────────────────────

#define LOG_BUF_SIZE 8192

class LogBuffer {
public:
    char buf[LOG_BUF_SIZE];
    int len = 0;

    void clear() { len = 0; buf[0] = '\0'; }

    // Write a formatted log entry with uptime timestamp
    void write(uint32_t uptimeMs, const char* fmt, ...) {
        int avail = LOG_BUF_SIZE - len;

        // Timestamp
        if (avail > 16) {
            int n = snprintf(buf + len, avail, "[+%lus] ", (unsigned long)(uptimeMs / 1000));
            len += n;
            avail -= n;
        }

        // Message
        if (avail > 1) {
            va_list args;
            va_start(args, fmt);
            int n = vsnprintf(buf + len, avail, fmt, args);
            va_end(args);
            if (n > 0) len += (n < avail) ? n : avail - 1;
        }

        // Newline
        if (len < LOG_BUF_SIZE - 1) buf[len++] = '\n';
        buf[len] = '\0';

        // Discard oldest half if full
        if (len > LOG_BUF_SIZE - 256) {
            int half = len / 2;
            while (half < len && buf[half] != '\n') half++;
            if (half < len) half++;
            memmove(buf, buf + half, len - half);
            len -= half;
            buf[len] = '\0';
        }
    }

    // Get buffer contents as string
    std::string contents() const { return std::string(buf, len); }
};

// ── CLI STATUS formatting ───────────────────────────────────────────────────

struct DeviceStatus {
    // Network
    bool pppConnected;
    bool wifiConnected;
    int  modemRssi;
    char modemOp[32];
    // Upload
    uint16_t filesQueued;
    uint16_t filesUploaded;
    float mbQueued;
    float mbUploaded;
    float lastUploadKBps;
    // USB/harvest
    float hostWrittenMb;
    bool harvesting;
    // S3
    char apiHost[128];
    char deviceId[24];   // fits "TEST_<12hex>" (17) in e2e builds
    // Firmware
    char fwVersion[16];
    bool mscOnly;
};

// Format device status as multi-line text (same format as CLI STATUS command).
inline std::string formatStatus(const DeviceStatus& s) {
    std::string out;
    char line[256];

    snprintf(line, sizeof(line), "files_q=%u files_up=%u mbq=%.2f mbup=%.2f",
             s.filesQueued, s.filesUploaded, s.mbQueued, s.mbUploaded);
    out += line;
    out += "\n";

    if (s.lastUploadKBps > 0) {
        snprintf(line, sizeof(line), "last_upload=%.0f KB/s", s.lastUploadKBps);
        out += line;
        out += "\n";
    }

    snprintf(line, sizeof(line), "net=%s rssi=%d op=%s",
             s.pppConnected ? "cellular" : (s.wifiConnected ? "wifi" : "none"),
             s.modemRssi, s.modemOp);
    out += line;
    out += "\n";

    snprintf(line, sizeof(line), "wr_mb=%.2f harvesting=%d",
             s.hostWrittenMb, s.harvesting);
    out += line;
    out += "\n";

    snprintf(line, sizeof(line), "s3 api_host=%s device=%s",
             s.apiHost, s.deviceId);
    out += line;
    out += "\n";

    snprintf(line, sizeof(line), "fw=%s usb=%s",
             s.fwVersion, s.mscOnly ? "MSC-only" : "CDC+MSC");
    out += line;
    out += "\n";

    return out;
}

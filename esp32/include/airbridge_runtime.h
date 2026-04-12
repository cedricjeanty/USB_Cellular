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

// Check for OTA firmware update via the S3 API.
// Does NOT download or flash — just checks version and gets URL.
inline OtaCheckResult halOtaCheck(const char* currentVersion) {
    OtaCheckResult r = {};
    if (!g_hal || !g_hal->network || !g_hal->nvs) { r.status = -1; return r; }

    S3Creds creds = loadS3Creds();
    if (!creds.valid) { r.status = 0; return r; }  // no creds = don't retry

    // GET /prod/firmware — version check
    std::string verResp = s3ApiGetViaHal(creds.apiHost, creds.apiKey, "");
    // The API path is /prod/firmware, not /prod/presign — we need a different path.
    // Use a direct GET with the firmware path:
    TlsHandle tls = g_hal->network->connect(creds.apiHost);
    if (!tls) { r.status = -1; return r; }

    char req[512];
    snprintf(req, sizeof(req),
        "GET /prod/firmware HTTP/1.1\r\nHost: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
        creds.apiHost, creds.apiKey);
    if (!g_hal->network->write(tls, req, strlen(req))) {
        g_hal->network->destroy(tls);
        r.status = -1;
        return r;
    }
    verResp = halHttpReadResponse(tls);
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
        "GET /prod/firmware/download HTTP/1.1\r\nHost: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
        creds.apiHost, creds.apiKey);
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
    char deviceId[16];
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

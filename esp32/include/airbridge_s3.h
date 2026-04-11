#pragma once
// AirBridge S3 upload — credential loading, file upload (single + multipart),
// session management with NVS resume/retry.
// All I/O through HAL interfaces — works on ESP32 and native emulator.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_http.h"
#include <cstdio>
#include <cstring>
#include <string>

#define S3_CHUNK_SIZE (5UL * 1024 * 1024)

// Forward declaration
inline void clearMultipartSession();

struct MultipartSession {
    char uploadId[256];
    char key[128];
    uint32_t startPart;
    uint32_t totalParts;
    bool isResume;       // true if resuming existing session
    bool cleared;        // true if stale session was cleared
};

// Load or initialize a multipart upload session from NVS.
// Returns false if a stale session was cleared (caller should start fresh).
inline MultipartSession loadMultipartSession(const char* filename) {
    MultipartSession s = {};
    if (!g_hal || !g_hal->nvs) return s;

    char storedName[64] = "";
    g_hal->nvs->get_str("s3up", "name", storedName, sizeof(storedName));

    if (strcmp(storedName, filename) == 0) {
        uint32_t retries = 0;
        g_hal->nvs->get_u32("s3up", "retries", &retries);

        if (retries >= 3) {
            // Too many resume failures — clear session
            s.cleared = true;
            clearMultipartSession();
            return s;
        }

        // Resume existing session
        g_hal->nvs->get_str("s3up", "uid", s.uploadId, sizeof(s.uploadId));
        g_hal->nvs->get_str("s3up", "key", s.key, sizeof(s.key));
        g_hal->nvs->get_u32("s3up", "part", &s.startPart);
        g_hal->nvs->get_u32("s3up", "parts", &s.totalParts);
        g_hal->nvs->set_u32("s3up", "retries", retries + 1);
        s.isResume = true;
    }
    return s;
}

// Save a new multipart session to NVS.
inline void saveMultipartSession(const char* filename, const char* uploadId,
                                  const char* key, uint32_t totalParts, uint32_t fileSize) {
    if (!g_hal || !g_hal->nvs) return;
    g_hal->nvs->set_str("s3up", "name", filename);
    g_hal->nvs->set_str("s3up", "uid", uploadId);
    g_hal->nvs->set_str("s3up", "key", key);
    g_hal->nvs->set_u32("s3up", "part", 1);
    g_hal->nvs->set_u32("s3up", "parts", totalParts);
    g_hal->nvs->set_u32("s3up", "size", fileSize);
    g_hal->nvs->set_u32("s3up", "retries", 0);
}

// Save progress after a part is uploaded successfully.
inline void savePartProgress(uint32_t partNum, const char* etag) {
    if (!g_hal || !g_hal->nvs) return;
    char etagKey[12];
    snprintf(etagKey, sizeof(etagKey), "etag%u", partNum);
    g_hal->nvs->set_str("s3up", etagKey, etag);
    g_hal->nvs->set_u32("s3up", "part", partNum + 1);
    g_hal->nvs->set_u32("s3up", "retries", 0);
}

// Build the parts JSON for multipart completion: [{"part":1,"etag":"abc"},...]
inline std::string buildPartsJson(uint32_t totalParts) {
    std::string json;
    if (!g_hal || !g_hal->nvs) return json;
    for (uint32_t i = 1; i <= totalParts; i++) {
        char etagKey[12], etag[64] = "";
        snprintf(etagKey, sizeof(etagKey), "etag%u", i);
        g_hal->nvs->get_str("s3up", etagKey, etag, sizeof(etag));
        if (i > 1) json += ",";
        char part[128];
        snprintf(part, sizeof(part), "{\"part\":%u,\"etag\":\"%s\"}", i, etag);
        json += part;
    }
    return json;
}

// Clear the multipart session from NVS.
inline void clearMultipartSession() {
    if (!g_hal || !g_hal->nvs) return;
    g_hal->nvs->erase_key("s3up", "name");
    g_hal->nvs->erase_key("s3up", "uid");
    g_hal->nvs->erase_key("s3up", "key");
    g_hal->nvs->erase_key("s3up", "part");
    g_hal->nvs->erase_key("s3up", "parts");
    g_hal->nvs->erase_key("s3up", "size");
    g_hal->nvs->erase_key("s3up", "retries");
}

// Upload progress callback: called with bytes sent so far
typedef void (*UploadProgressFn)(uint32_t bytesSent, uint32_t totalBytes);

// ── Find next file to upload ─────────────────────────────────────────────────

// Scan harvestDir for the next file that needs uploading (no .done__ marker).
// Returns filename in `out`, or empty string if nothing to upload.
inline bool findNextUploadFile(const char* harvestDir, char* out, size_t outSz) {
    out[0] = '\0';
    if (!g_hal || !g_hal->filesys) return false;

    void* dir = g_hal->filesys->opendir(harvestDir);
    if (!dir) return false;

    FsDirEntry ent;
    while (g_hal->filesys->readdir(dir, &ent)) {
        if (ent.is_dir) continue;
        if (ent.name[0] == '.') continue;
        if (strcmp(ent.name, "airbridge.log") == 0) continue;

        // Skip 0-byte files
        char fullpath[192];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", harvestDir, ent.name);
        uint32_t sz = 0; bool isDir = false;
        if (g_hal->filesys->stat(fullpath, &sz, &isDir) && sz == 0) continue;

        // Skip if .done__ marker exists
        char donePath[256];
        snprintf(donePath, sizeof(donePath), "%s/.done__%s", harvestDir, ent.name);
        if (g_hal->filesys->exists(donePath)) continue;

        strlcpy(out, ent.name, outSz);
        break;
    }
    g_hal->filesys->closedir(dir);
    return out[0] != '\0';
}

// Mark a file as uploaded: delete the harvested copy and create .done__ marker.
inline void markFileUploaded(const char* harvestDir, const char* filename) {
    if (!g_hal || !g_hal->filesys) return;
    char path[192], donePath[256];
    snprintf(path, sizeof(path), "%s/%s", harvestDir, filename);
    snprintf(donePath, sizeof(donePath), "%s/.done__%s", harvestDir, filename);
    g_hal->filesys->remove(path);
    void* f = g_hal->filesys->open(donePath, "w");
    if (f) g_hal->filesys->close(f);
}

// ── S3 credentials ──────────────────────────────────────────────────────────

struct S3Creds {
    char apiHost[128];
    char apiKey[64];
    char deviceId[16];
    bool valid;
};

inline S3Creds loadS3Creds() {
    S3Creds c = {};
    if (!g_hal || !g_hal->nvs) return c;
    g_hal->nvs->get_str("s3", "api_host", c.apiHost, sizeof(c.apiHost));
    g_hal->nvs->get_str("s3", "api_key", c.apiKey, sizeof(c.apiKey));
    g_hal->nvs->get_str("s3", "device_id", c.deviceId, sizeof(c.deviceId));
    c.valid = (c.apiHost[0] && c.apiKey[0]);
    return c;
}

// ── Stream file to TLS via HAL ──────────────────────────────────────────────

inline bool halStreamFile(TlsHandle tls, void* fileHandle, uint32_t len,
                          UploadProgressFn progress = nullptr) {
    if (!g_hal || !g_hal->network || !g_hal->filesys) return false;
    uint8_t buf[8192];
    uint32_t remaining = len;
    uint32_t sent = 0;
    while (remaining > 0) {
        uint32_t toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        size_t n = g_hal->filesys->read(fileHandle, buf, toRead);
        if (n == 0) return false;
        if (!g_hal->network->write(tls, buf, n)) return false;
        remaining -= n;
        sent += n;
        if (progress) progress(sent, len);
    }
    return true;
}

// ── Full single-file upload via HAL ─────────────────────────────────────────

struct UploadResult {
    bool success;
    float kbps;          // upload speed
    char error[128];     // error message if !success
};

// Upload a single file to S3 using pre-signed URLs.
// filepath: full path to the file (e.g. "./emu_sdcard/harvested/data.csv")
// filename: just the name (e.g. "data.csv") — used for S3 key
// progress: optional callback for display updates
inline UploadResult halS3UploadFile(const char* filepath, const char* filename,
                                     UploadProgressFn progress = nullptr) {
    UploadResult res = {};
    if (!g_hal || !g_hal->network || !g_hal->filesys || !g_hal->nvs) {
        strlcpy(res.error, "HAL not initialized", sizeof(res.error));
        return res;
    }

    S3Creds creds = loadS3Creds();
    if (!creds.valid) {
        strlcpy(res.error, "No S3 credentials", sizeof(res.error));
        return res;
    }

    // Get file size
    uint32_t fileSize = 0;
    bool isDir = false;
    if (!g_hal->filesys->stat(filepath, &fileSize, &isDir) || fileSize == 0) {
        snprintf(res.error, sizeof(res.error), "Can't stat %s", filepath);
        return res;
    }

    // Get pre-signed URL
    char query[512];
    snprintf(query, sizeof(query), "file=%s&size=%u&device=%s",
             urlEncode(filename).c_str(), fileSize, creds.deviceId);
    std::string resp = s3ApiGetViaHal(creds.apiHost, creds.apiKey, query);
    std::string url = jsonStr(resp, "url");
    if (url.empty()) {
        snprintf(res.error, sizeof(res.error), "Presign failed");
        return res;
    }

    // Parse S3 URL
    char s3Host[128], s3Path[2500];
    if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
        strlcpy(res.error, "URL parse failed", sizeof(res.error));
        return res;
    }

    // Open file
    void* f = g_hal->filesys->open(filepath, "rb");
    if (!f) {
        snprintf(res.error, sizeof(res.error), "Can't open %s", filepath);
        return res;
    }

    // Connect to S3
    TlsHandle tls = g_hal->network->connect(s3Host);
    if (!tls) {
        g_hal->filesys->close(f);
        strlcpy(res.error, "TLS connect failed", sizeof(res.error));
        return res;
    }

    // Send PUT header
    char hdr[2700];
    snprintf(hdr, sizeof(hdr),
        "PUT %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
        s3Path, s3Host, fileSize);
    if (!g_hal->network->write(tls, hdr, strlen(hdr))) {
        g_hal->network->destroy(tls);
        g_hal->filesys->close(f);
        strlcpy(res.error, "Header send failed", sizeof(res.error));
        return res;
    }

    // Stream file
    uint32_t startMs = g_hal->clock->millis();
    bool streamed = halStreamFile(tls, f, fileSize, progress);
    g_hal->filesys->close(f);

    if (!streamed) {
        g_hal->network->destroy(tls);
        strlcpy(res.error, "Stream failed", sizeof(res.error));
        return res;
    }

    // Read response
    halHttpReadResponse(tls);
    g_hal->network->destroy(tls);

    uint32_t elapsed = g_hal->clock->millis() - startMs;
    res.success = true;
    res.kbps = elapsed > 0 ? fileSize / 1024.0f / (elapsed / 1000.0f) : 0;
    return res;
}

// ── Upload all pending files ────────────────────────────────────────────────

// Upload all pending files from harvestDir. Returns count of files uploaded.
inline int uploadAllFiles(const char* harvestDir, UploadProgressFn progress = nullptr) {
    int count = 0;
    char name[64];
    while (findNextUploadFile(harvestDir, name, sizeof(name))) {
        char fullpath[192];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", harvestDir, name);

        UploadResult r = halS3UploadFile(fullpath, name, progress);
        if (r.success) {
            markFileUploaded(harvestDir, name);
            count++;
        } else {
            break;
        }
    }
    return count;
}

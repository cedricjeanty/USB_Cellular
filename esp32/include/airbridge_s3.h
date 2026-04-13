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

// Scan harvestDir for numbered subfolders (0001, 0002, ...), return the first
// file from the oldest non-empty subfolder.  `out` receives "NNNN/filename".
// Empty subfolders are removed automatically.
inline bool findNextUploadFile(const char* harvestDir, char* out, size_t outSz) {
    out[0] = '\0';
    if (!g_hal || !g_hal->filesys) return false;

    // Collect all numeric subfolder names
    char subs[32][16];
    int nSubs = 0;
    void* topDir = g_hal->filesys->opendir(harvestDir);
    if (!topDir) return false;
    FsDirEntry ent;
    while (g_hal->filesys->readdir(topDir, &ent) && nSubs < 32) {
        if (!ent.is_dir || ent.name[0] == '.') continue;
        bool numeric = true;
        for (const char* p = ent.name; *p && numeric; p++)
            if (*p < '0' || *p > '9') numeric = false;
        if (!numeric || ent.name[0] == '\0') continue;
        strlcpy(subs[nSubs++], ent.name, 16);
    }
    g_hal->filesys->closedir(topDir);

    // Sort ascending (oldest first)
    for (int i = 0; i < nSubs - 1; i++)
        for (int j = i + 1; j < nSubs; j++)
            if (strcmp(subs[i], subs[j]) > 0) {
                char tmp[16]; strlcpy(tmp, subs[i], 16);
                strlcpy(subs[i], subs[j], 16); strlcpy(subs[j], tmp, 16);
            }

    // Find first file in any subfolder (skip + rmdir empty ones)
    for (int s = 0; s < nSubs; s++) {
        char subPath[256];
        snprintf(subPath, sizeof(subPath), "%s/%s", harvestDir, subs[s]);
        void* subDir = g_hal->filesys->opendir(subPath);
        if (!subDir) continue;

        bool found = false;
        while (g_hal->filesys->readdir(subDir, &ent)) {
            if (ent.is_dir || ent.name[0] == '.') continue;
            char fullpath[256];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", subPath, ent.name);
            uint32_t sz = 0; bool isDir = false;
            if (g_hal->filesys->stat(fullpath, &sz, &isDir) && sz == 0) continue;
            snprintf(out, outSz, "%s/%s", subs[s], ent.name);
            found = true;
            break;
        }
        g_hal->filesys->closedir(subDir);

        if (found) return true;
        // Empty subfolder — remove it and try next
        g_hal->filesys->rmdir(subPath);
    }
    return false;
}

// Delete an uploaded file and remove its subfolder if empty.
// relPath is "NNNN/filename" as returned by findNextUploadFile.
inline void markFileUploaded(const char* harvestDir, const char* relPath) {
    if (!g_hal || !g_hal->filesys) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", harvestDir, relPath);
    g_hal->filesys->remove(path);

    // Extract subfolder path and try to remove it (succeeds only if empty)
    const char* slash = strchr(relPath, '/');
    if (slash) {
        char sub[256];
        size_t subLen = slash - relPath;
        snprintf(sub, sizeof(sub), "%s/%.*s", harvestDir, (int)subLen, relPath);
        g_hal->filesys->rmdir(sub);
    }
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
    int throttle = g_hal->network->getMaxBytesPerSec();
    while (remaining > 0) {
        uint32_t toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        if (throttle > 0) {
            uint32_t maxChunk = throttle / 10;  // 100ms worth
            if (maxChunk < 1024) maxChunk = 1024;
            if (toRead > maxChunk) toRead = maxChunk;
        }
        size_t n = g_hal->filesys->read(fileHandle, buf, toRead);
        if (n == 0) return false;
        if (!g_hal->network->write(tls, buf, n)) return false;
        remaining -= n;
        sent += n;
        if (progress) progress(sent, len);
        if (throttle > 0 && g_hal->clock) {
            g_hal->clock->delay_ms(n * 1000 / throttle);
        }
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

    // Presign request
    char query[512];
    snprintf(query, sizeof(query), "file=%s&size=%u&device=%s",
             urlEncode(filename).c_str(), fileSize, creds.deviceId);
    std::string resp = s3ApiGetViaHal(creds.apiHost, creds.apiKey, query);

    std::string url = jsonStr(resp, "url");
    std::string uploadId = jsonStr(resp, "upload_id");
    int totalParts = jsonInt(resp, "parts");
    std::string s3Key = jsonStr(resp, "key");

    // Open file
    void* f = g_hal->filesys->open(filepath, "rb");
    if (!f) {
        snprintf(res.error, sizeof(res.error), "Can't open %s", filepath);
        return res;
    }

    uint32_t startMs = g_hal->clock->millis();

    // ── Single-part upload (small files, url is set) ────────────────────
    if (url.length() > 0 && uploadId.empty()) {
        char s3Host[128], s3Path[2500];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            g_hal->filesys->close(f);
            strlcpy(res.error, "URL parse failed", sizeof(res.error));
            return res;
        }

        TlsHandle tls = g_hal->network->connect(s3Host);
        if (!tls) {
            g_hal->filesys->close(f);
            strlcpy(res.error, "TLS connect failed", sizeof(res.error));
            return res;
        }

        char hdr[2700];
        snprintf(hdr, sizeof(hdr),
            "PUT %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            s3Path, s3Host, fileSize);
        if (!g_hal->network->write(tls, hdr, strlen(hdr))) {
            g_hal->network->destroy(tls); g_hal->filesys->close(f);
            strlcpy(res.error, "Header send failed", sizeof(res.error));
            return res;
        }

        bool ok = halStreamFile(tls, f, fileSize, progress);
        g_hal->filesys->close(f);
        std::string putResp = halHttpReadResponse(tls);
        g_hal->network->destroy(tls);

        if (!ok) { strlcpy(res.error, "Stream failed", sizeof(res.error)); return res; }
        // Check for S3 error in response
        if (putResp.find("Error") != std::string::npos || putResp.find("error") != std::string::npos) {
            snprintf(res.error, sizeof(res.error), "S3 error: %.100s", putResp.c_str());
            return res;
        }

        uint32_t elapsed = g_hal->clock->millis() - startMs;
        res.success = true;
        res.kbps = elapsed > 0 ? fileSize / 1024.0f / (elapsed / 1000.0f) : 0;
        return res;
    }

    // ── Multipart upload (large files, upload_id is set) ────────────────
    if (uploadId.empty() || s3Key.empty() || totalParts <= 0) {
        g_hal->filesys->close(f);
        snprintf(res.error, sizeof(res.error), "Presign failed");
        return res;
    }

    // Check for NVS resume session
    MultipartSession session = loadMultipartSession(filename);
    uint32_t startPart = 1;
    if (session.isResume) {
        startPart = session.startPart;
        strlcpy((char*)uploadId.c_str(), session.uploadId, sizeof(session.uploadId));
    } else {
        saveMultipartSession(filename, uploadId.c_str(), s3Key.c_str(), totalParts, fileSize);
    }

    // Seek to resume position
    if (startPart > 1) {
        g_hal->filesys->seek(f, (startPart - 1) * S3_CHUNK_SIZE, 0 /*SEEK_SET*/);
    }

    for (uint32_t partNum = startPart; partNum <= (uint32_t)totalParts; partNum++) {
        uint32_t offset = (partNum - 1) * S3_CHUNK_SIZE;
        uint32_t chunkSize = fileSize - offset;
        if (chunkSize > S3_CHUNK_SIZE) chunkSize = S3_CHUNK_SIZE;

        // Get presigned URL for this part
        char pq[1024];
        snprintf(pq, sizeof(pq), "upload_id=%s&key=%s&part=%u",
                 uploadId.c_str(), urlEncode(s3Key.c_str()).c_str(), partNum);
        std::string partResp = s3ApiGetViaHal(creds.apiHost, creds.apiKey, pq);
        std::string partUrl = jsonStr(partResp, "url");
        if (partUrl.empty()) {
            g_hal->filesys->close(f);
            snprintf(res.error, sizeof(res.error), "Presign part %u failed", partNum);
            return res;
        }

        char s3Host[128], s3Path[2500];
        if (!parseUrl(partUrl, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            g_hal->filesys->close(f);
            strlcpy(res.error, "Part URL parse failed", sizeof(res.error));
            return res;
        }

        TlsHandle tls = g_hal->network->connect(s3Host);
        if (!tls) {
            g_hal->filesys->close(f);
            snprintf(res.error, sizeof(res.error), "TLS connect part %u failed", partNum);
            return res;
        }

        char hdr[2700];
        snprintf(hdr, sizeof(hdr),
            "PUT %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            s3Path, s3Host, chunkSize);
        if (!g_hal->network->write(tls, hdr, strlen(hdr)) ||
            !halStreamFile(tls, f, chunkSize, progress)) {
            g_hal->network->destroy(tls); g_hal->filesys->close(f);
            snprintf(res.error, sizeof(res.error), "Stream part %u failed", partNum);
            return res;
        }

        char etag[64] = "";
        halHttpReadResponse(tls, etag, sizeof(etag));
        g_hal->network->destroy(tls);

        if (etag[0]) savePartProgress(partNum, etag);
    }
    g_hal->filesys->close(f);

    // Complete multipart
    std::string partsJson = buildPartsJson(totalParts);
    std::string completeReq = buildApiCompleteRequest(
        creds.apiHost, creds.apiKey, uploadId.c_str(), s3Key.c_str(), partsJson.c_str());

    TlsHandle tls = g_hal->network->connect(creds.apiHost);
    if (tls) {
        g_hal->network->write(tls, completeReq.c_str(), completeReq.size());
        std::string cResp = halHttpReadResponse(tls);
        g_hal->network->destroy(tls);
    }
    clearMultipartSession();

    uint32_t elapsed = g_hal->clock->millis() - startMs;
    res.success = true;
    res.kbps = elapsed > 0 ? fileSize / 1024.0f / (elapsed / 1000.0f) : 0;
    return res;
}

// ── Upload all pending files ────────────────────────────────────────────────

// Upload all pending files from harvestDir. Returns count of files uploaded.
// findNextUploadFile returns "NNNN/filename" relative paths.
inline int uploadAllFiles(const char* harvestDir, UploadProgressFn progress = nullptr) {
    int count = 0;
    char relPath[128];
    while (findNextUploadFile(harvestDir, relPath, sizeof(relPath))) {
        char fullpath[256];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", harvestDir, relPath);

        // Use the relative path (NNNN/filename) as the S3 key component
        UploadResult r = halS3UploadFile(fullpath, relPath, progress);
        if (r.success) {
            markFileUploaded(harvestDir, relPath);
            count++;
        } else {
            break;
        }
    }
    return count;
}

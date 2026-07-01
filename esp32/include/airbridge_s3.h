#pragma once
// AirBridge S3 upload — credential loading, file upload (single + multipart),
// session management with NVS resume/retry.
// All I/O through HAL interfaces — works on ESP32 and native emulator.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_http.h"
#include "airbridge_log.h"   // ULDBG breadcrumbs reach CDC+S3 on firmware, stdout on emulator
#include <cstdio>
#include <cstring>
#include <string>

// Upload-path trace breadcrumbs. Routed through airbridge_log — unlike the raw
// printf() debug in this file, which on firmware goes to UART0, NOT the captured
// USB CDC (which is why those never showed up). Diagnostic for the large-file
// upload hang; compile out with -DUL_TRACE=0 once root-caused.
#ifndef UL_TRACE
#define UL_TRACE 1
#endif
#if UL_TRACE
#define ULDBG(...) airbridge_log(__VA_ARGS__)
#else
#define ULDBG(...) ((void)0)
#endif

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
        uint32_t retries = 0, startPart = 1;
        g_hal->nvs->get_u32("s3up", "retries", &retries);
        g_hal->nvs->get_u32("s3up", "part", &startPart);   // next part (1-based)

        // Abandon only a session that has made NO real progress. A session with completed
        // parts (startPart > 1) is real uploaded data — on a marginal cellular link it may
        // take several windows to land each part, and `retries` resets on every completed
        // part, so a high limit here lets it keep resuming the SAME multipart instead of
        // discarding 10s of MB and starting fresh (which strands the parts as an orphan).
        uint32_t limit = (startPart > 1) ? 50 : 3;
        if (retries >= limit) {
            s.cleared = true;
            clearMultipartSession();
            return s;
        }

        // Resume existing session
        g_hal->nvs->get_str("s3up", "uid", s.uploadId, sizeof(s.uploadId));
        g_hal->nvs->get_str("s3up", "key", s.key, sizeof(s.key));
        s.startPart = startPart;
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
        // Collect all uploadable files, then pick the one with the lowest
        // flight number (for .eaofh) so the manifest hwm advances correctly.
        char bestName[128] = "";
        uint32_t bestFlight = UINT32_MAX;
        while (g_hal->filesys->readdir(subDir, &ent)) {
            if (ent.is_dir || ent.name[0] == '.') continue;
            size_t nlen = strlen(ent.name);
            if (nlen > 5 && strcmp(ent.name + nlen - 5, ".meta") == 0) continue;
            char fullpath[256];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", subPath, ent.name);
            uint32_t sz = 0; bool isDir = false;
            if (g_hal->filesys->stat(fullpath, &sz, &isDir) && sz == 0) continue;

            // For .eaofh files, sort ascending by flight number
            const char* ext = strrchr(ent.name, '.');
            if (ext && strcmp(ext, ".eaofh") == 0) {
                // Strip harvest prefix to find bare name for flight parse
                const char* bare = ent.name;
                for (const char* p = ent.name; p[0] && p[1]; p++)
                    if (p[0] == '_' && p[1] == '_') bare = p + 2;
                char serial[44]; uint32_t lastFlight = 0;
                if (parseEaofhFilename(bare, serial, sizeof(serial), &lastFlight)) {
                    if (lastFlight < bestFlight) {
                        bestFlight = lastFlight;
                        strlcpy(bestName, ent.name, sizeof(bestName));
                    }
                    continue;
                }
            }
            // Non-.eaofh: take immediately (no sort needed)
            if (!found) {
                snprintf(out, outSz, "%s/%s", subs[s], ent.name);
                found = true;
            }
        }
        // Use lowest-flight .eaofh if found (and no regular file was selected)
        if (!found && bestName[0]) {
            snprintf(out, outSz, "%s/%s", subs[s], bestName);
            found = true;
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
    char deviceId[24];   // 12-hex MAC id, or "TEST_<12hex>" (17) in e2e builds
    bool valid;
};

// Optional fallback the FIRMWARE installs so credentials never depend on NVS being
// intact (the 20 KB NVS can lose entries under power-cut churn — the power-cut-soak
// failure where uploads stall on "No S3 credentials"). It fills any empty field from
// always-reconstructable sources: the shared compiled api host/key + a MAC-derived
// device_id. The emulator/tests leave it null, so their empty-NVS semantics (a test
// asserts loadS3Creds() is invalid on empty NVS) are unchanged.
typedef void (*S3CredsFallbackFn)(S3Creds* c);
inline S3CredsFallbackFn& s3CredsFallback() { static S3CredsFallbackFn fn = nullptr; return fn; }

inline S3Creds loadS3Creds() {
    S3Creds c = {};
    if (g_hal && g_hal->nvs) {
        g_hal->nvs->get_str("s3", "api_host", c.apiHost, sizeof(c.apiHost));
        g_hal->nvs->get_str("s3", "api_key", c.apiKey, sizeof(c.apiKey));
        g_hal->nvs->get_str("s3", "device_id", c.deviceId, sizeof(c.deviceId));
    }
    if ((!c.apiHost[0] || !c.apiKey[0] || !c.deviceId[0]) && s3CredsFallback())
        s3CredsFallback()(&c);
    c.valid = (c.apiHost[0] && c.apiKey[0]);
    return c;
}

// ── Stream file to TLS via HAL ──────────────────────────────────────────────

inline bool halStreamFile(TlsHandle tls, void* fileHandle, uint32_t len,
                          UploadProgressFn progress = nullptr) {
    if (!g_hal || !g_hal->network || !g_hal->filesys) return false;
    static uint8_t buf[8192];  // static: keep off the (small) firmware task stack
    uint32_t remaining = len;
    uint32_t sent = 0;
    int throttle = g_hal->network->getMaxBytesPerSec();
    ULDBG("ULDBG stream begin len=%u", len);
    uint32_t traceMb = 0;  // emit one rd/wr breadcrumb pair per MB
    while (remaining > 0) {
        uint32_t toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        if (throttle > 0) {
            uint32_t maxChunk = throttle / 10;  // 100ms worth
            if (maxChunk < 1024) maxChunk = 1024;
            if (toRead > maxChunk) toRead = maxChunk;
        }
        bool trace = (sent >> 20) >= traceMb;
        // Hold the SD lock only around the read — the slow TLS write runs
        // unlocked so the USB MSC task isn't starved (matches live httpStreamChunk).
        if (trace) ULDBG("ULDBG rd @%u/%u", sent, len);   // last line if SD read stalls
        g_hal->filesys->lock();
        size_t n = g_hal->filesys->read(fileHandle, buf, toRead);
        g_hal->filesys->unlock();
        if (n == 0) { ULDBG("ULDBG read=0 @%u", sent); return false; }
        if (trace) ULDBG("ULDBG wr @%u n=%u", sent, (unsigned)n);  // last line if TLS write stalls
        if (!g_hal->network->write(tls, buf, n)) { ULDBG("ULDBG write fail @%u", sent); return false; }
        remaining -= n;
        sent += n;
        if (trace) traceMb = (sent >> 20) + 1;
        if (progress) progress(sent, len);
        if (throttle > 0 && g_hal->clock) {
            g_hal->clock->delay_ms(n * 1000 / throttle);
        }
    }
    ULDBG("ULDBG stream done %u", sent);
    return true;
}

// ── Full single-file upload via HAL ─────────────────────────────────────────

struct UploadResult {
    bool success;
    float kbps;          // upload speed
    char error[128];     // error message if !success
};

// Upload a single file to S3 using pre-signed URLs.
// filepath: full path to the file
// filename: used in the presign query (file= param); also default S3 key component
// keyOverride: if non-null, sent as key= param to use as the full S3 key
// progress: optional callback for display updates
inline UploadResult halS3UploadFile(const char* filepath, const char* filename,
                                     UploadProgressFn progress = nullptr,
                                     const char* keyOverride = nullptr) {
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
    char query[640];
    if (keyOverride) {
        std::string encKey = urlEncode(keyOverride);
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s&key=%s",
                 urlEncode(filename).c_str(), fileSize, creds.deviceId, encKey.c_str());
    } else {
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s",
                 urlEncode(filename).c_str(), fileSize, creds.deviceId);
    }
    ULDBG("ULDBG halS3UploadFile %s sz=%u presign...", filename, fileSize);
    std::string resp = s3ApiGetViaHal(creds.apiHost, creds.apiKey, query);
    ULDBG("ULDBG presign resp len=%u", (unsigned)resp.size());

    // Skip upload if S3 already has this file with matching size
    if (resp.find("\"skip\"") != std::string::npos &&
        resp.find("true") != std::string::npos) {
        res.success = true;
        res.kbps = 0;
        strlcpy(res.error, "skipped (already on S3)", sizeof(res.error));
        return res;
    }

    std::string url = jsonStr(resp, "url");
    std::string uploadId = jsonStr(resp, "upload_id");
    int totalParts = jsonInt(resp, "parts");
    std::string s3Key = jsonStr(resp, "key");
    ULDBG("ULDBG presign: url=%d uploadId=%d parts=%d", (int)!url.empty(), (int)!uploadId.empty(), totalParts);

    // Open file
    void* f = g_hal->filesys->open(filepath, "rb");
    if (!f) {
        snprintf(res.error, sizeof(res.error), "Can't open %s", filepath);
        return res;
    }

    uint32_t startMs = g_hal->clock->millis();

    // ── Single-part upload (small files, url is set) ────────────────────
    if (url.length() > 0 && uploadId.empty()) {
        ULDBG("ULDBG single-PUT path");
        char s3Host[128], s3Path[2500];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            g_hal->filesys->close(f);
            strlcpy(res.error, "URL parse failed", sizeof(res.error));
            return res;
        }

        ULDBG("ULDBG single-PUT connect %s", s3Host);
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
        char etag[64] = "";
        int status = 0;
        std::string putResp = halHttpReadResponse(tls, etag, sizeof(etag), &status);
        g_hal->network->destroy(tls);

        if (!ok) { strlcpy(res.error, "Stream failed", sizeof(res.error)); return res; }
        // VERIFY the PUT actually succeeded before declaring victory. A PUT cut short
        // (link drop / shutdown mid-upload) yields an empty/partial response — status 0,
        // no ETag — which previously slipped through as success and advanced the manifest
        // over a TRUNCATED S3 object (silent data loss). S3 returns 200 + an ETag on a
        // complete PUT; require both. Anything else fails → the upload task retries.
        if (status != 200 || !etag[0]) {
            snprintf(res.error, sizeof(res.error), "PUT not confirmed (HTTP %d, etag=%d)",
                     status, (int)(etag[0] != 0));
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

    // Bracket the whole multipart session (across all part PUTs + completion) so
    // the firmware suppresses the modem `+++` escape in the gaps between parts.
    // RAII → every return path below clears it. See INetwork::setUploadSession.
    struct UploadSessionGuard {
        UploadSessionGuard()  { if (g_hal && g_hal->network) g_hal->network->setUploadSession(true); }
        ~UploadSessionGuard() { if (g_hal && g_hal->network) g_hal->network->setUploadSession(false); }
    } _uploadSession;

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

    ULDBG("ULDBG multipart: %d parts from part %u uploadId=[%.40s] key=%s",
          totalParts, startPart, uploadId.c_str(), s3Key.c_str());
    for (uint32_t partNum = startPart; partNum <= (uint32_t)totalParts; partNum++) {
        uint32_t offset = (partNum - 1) * S3_CHUNK_SIZE;
        uint32_t chunkSize = fileSize - offset;
        if (chunkSize > S3_CHUNK_SIZE) chunkSize = S3_CHUNK_SIZE;

        // Upload this part with retries. A part is only "done" once S3 returns a
        // non-empty ETag — on a flaky cellular link the PUT can stream fully yet
        // get no/!2xx response. Retrying (re-presign + re-seek + re-stream) avoids
        // the old failure mode where an empty-ETag part was silently skipped and
        // the final "complete" then failed with a missing part. Each attempt
        // re-seeks to this part's offset (halStreamFile advances the file pos).
        char etag[64] = "";
        const int PART_RETRIES = 3;
        for (int attempt = 0; attempt < PART_RETRIES && !etag[0]; attempt++) {
            ULDBG("ULDBG part %u/%d sz=%u attempt %d presign...",
                  partNum, totalParts, chunkSize, attempt + 1);

            // Presigned URL for this part. device= so _pick_bucket routes correctly.
            char pq[1024];
            snprintf(pq, sizeof(pq), "upload_id=%s&key=%s&part=%u&device=%s",
                     uploadId.c_str(), urlEncode(s3Key.c_str()).c_str(), partNum,
                     creds.deviceId);
            std::string partResp = s3ApiGetViaHal(creds.apiHost, creds.apiKey, pq);
            std::string partUrl = jsonStr(partResp, "url");
            if (partUrl.empty()) { ULDBG("ULDBG part %u presign empty, retry", partNum); continue; }

            char s3Host[128], s3Path[2500];
            if (!parseUrl(partUrl, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
                ULDBG("ULDBG part %u URL parse failed, retry", partNum); continue;
            }

            TlsHandle tls = g_hal->network->connect(s3Host);
            if (!tls) { ULDBG("ULDBG part %u TLS connect failed, retry", partNum); continue; }

            // Seek to this part's offset (required on every attempt — a prior
            // attempt or part left the file pos elsewhere).
            g_hal->filesys->seek(f, (long)offset, 0 /*SEEK_SET*/);

            char hdr[2700];
            snprintf(hdr, sizeof(hdr),
                "PUT %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                s3Path, s3Host, chunkSize);
            if (!g_hal->network->write(tls, hdr, strlen(hdr)) ||
                !halStreamFile(tls, f, chunkSize, progress)) {
                g_hal->network->destroy(tls);
                ULDBG("ULDBG part %u stream failed, retry", partNum);
                continue;
            }

            std::string partResp2 = halHttpReadResponse(tls, etag, sizeof(etag));
            g_hal->network->destroy(tls);
            ULDBG("ULDBG part %u attempt %d etag=[%s] resp=%.60s", partNum, attempt + 1,
                  etag[0] ? etag : "EMPTY", partResp2.c_str());
        }

        if (!etag[0]) {
            // All retries failed to get an ETag — abort. Parts saved so far stay in
            // the NVS resume session, so the upload task's next attempt resumes.
            g_hal->filesys->close(f);
            snprintf(res.error, sizeof(res.error), "Part %u failed after %d retries (no ETag)",
                     partNum, PART_RETRIES);
            return res;
        }
        savePartProgress(partNum, etag);
    }
    g_hal->filesys->close(f);

    // Complete multipart
    std::string partsJson = buildPartsJson(totalParts);
    std::string completeReq = buildApiCompleteRequest(
        creds.apiHost, creds.apiKey, uploadId.c_str(), s3Key.c_str(),
        partsJson.c_str(), creds.deviceId);

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

// ── Fleet-aware manifest helpers ─────────────────────────────────────────────

// GET /prod/aircraft/manifest?serial=X → high_water_mark (0 on error/absent)
inline uint32_t halFetchManifest(const char* serial) {
    if (!g_hal || !g_hal->network) return 0;
    S3Creds creds = loadS3Creds();
    if (!creds.valid) return 0;
    char path[256];
    snprintf(path, sizeof(path), "/prod/aircraft/manifest?serial=%s",
             urlEncode(serial).c_str());
    std::string resp = s3ApiGetPathViaHal(creds.apiHost, creds.apiKey, path);
    int32_t hwm = jsonInt(resp, "high_water_mark");
    return (hwm > 0) ? (uint32_t)hwm : 0;
}

// POST /prod/aircraft/manifest → update manifest with new file entry.
inline bool halUpdateManifest(const char* serial, uint32_t firstFlight,
                              uint32_t lastFlight, const char* s3Key) {
    if (!g_hal || !g_hal->network) return false;
    S3Creds creds = loadS3Creds();
    if (!creds.valid) return false;
    char body[512];
    int bodyLen = snprintf(body, sizeof(body),
        "{\"serial\":\"%s\",\"last_flight\":%lu,\"first_flight\":%lu,\"s3_key\":\"%s\"}",
        serial, (unsigned long)lastFlight, (unsigned long)firstFlight, s3Key ? s3Key : "");
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "POST /prod/aircraft/manifest HTTP/1.1\r\n"
        "Host: %s\r\nx-api-key: %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        creds.apiHost, creds.apiKey, bodyLen);
    TlsHandle tls = g_hal->network->connect(creds.apiHost);
    if (!tls) return false;
    bool ok = g_hal->network->write(tls, hdr, strlen(hdr)) &&
              g_hal->network->write(tls, body, bodyLen);
    if (ok) {
        std::string resp = halHttpReadResponse(tls);
        ok = resp.find("high_water_mark") != std::string::npos;
        if (!ok) printf("[Manifest] POST failed, resp: %.200s\n", resp.c_str());
        else      printf("[Manifest] POST OK hwm advancing to %lu\n", (unsigned long)lastFlight);
    } else {
        printf("[Manifest] POST write failed (TLS error)\n");
    }
    g_hal->network->destroy(tls);
    return ok;
}

// ── Remote command channel (S3 over cellular) ───────────────────────────────
// Fetch a one-shot remote command (airbridge.cmd syntax). GET /prod/command?device=X
// → {"cmd":"<text>",...}; the Lambda deletes the object after the GET (run-once
// delivery). Copies the command text into outText; returns true if one was returned.
// The caller runs it through the SAME runCommandTextBuffer() as a USB airbridge.cmd.
inline bool halFetchCommands(const char* device, char* outText, size_t outSz) {
    if (!g_hal || !g_hal->network || !device || !outText || outSz == 0) return false;
    S3Creds creds = loadS3Creds();
    if (!creds.valid) return false;
    char path[256];
    snprintf(path, sizeof(path), "/prod/command?device=%s", urlEncode(device).c_str());
    std::string resp = s3ApiGetPathViaHal(creds.apiHost, creds.apiKey, path);
    std::string cmd = jsonStr(resp, "cmd");
    if (cmd.empty()) { outText[0] = '\0'; return false; }
    // JSON-unescape: the value arrives with \n (newlines between directives), \t, \"
    // etc. ESCAPED — decode them so parseCommands sees real newlines and verbs match.
    size_t o = 0;
    for (size_t i = 0; i < cmd.size() && o + 1 < outSz; i++) {
        char c = cmd[i];
        if (c == '\\' && i + 1 < cmd.size()) {
            char n = cmd[++i];
            switch (n) {
                case 'n': c = '\n'; break;  case 'r': c = '\r'; break;
                case 't': c = '\t'; break;  case '"': c = '"';  break;
                case '\\': c = '\\'; break; case '/': c = '/';  break;
                default:   c = n;    break;
            }
        }
        outText[o++] = c;
    }
    outText[o] = '\0';
    return o > 0;
}

// POST the command execution result (acceptance confirmation) — the Lambda stores it at
// commands/{device}/ack.json for the operator. resultJson is a compact JSON string.
// Sent BEFORE any reboot/format restart so a restart can't swallow the confirmation.
inline bool halAckCommand(const char* device, const char* resultJson) {
    if (!g_hal || !g_hal->network || !device || !resultJson) return false;
    S3Creds creds = loadS3Creds();
    if (!creds.valid) return false;
    int bodyLen = (int)strlen(resultJson);
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "POST /prod/command/ack?device=%s HTTP/1.1\r\n"
        "Host: %s\r\nx-api-key: %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        urlEncode(device).c_str(), creds.apiHost, creds.apiKey, bodyLen);
    TlsHandle tls = g_hal->network->connect(creds.apiHost);
    if (!tls) return false;
    bool ok = g_hal->network->write(tls, hdr, strlen(hdr)) &&
              g_hal->network->write(tls, resultJson, (size_t)bodyLen);
    if (ok) { std::string resp = halHttpReadResponse(tls); ok = resp.find("\"ok\"") != std::string::npos; }
    g_hal->network->destroy(tls);
    return ok;
}

// POST an always-on telemetry heartbeat — the Lambda stores it latest-wins at
// heartbeat/{device}.json so an operator can always SEE a unit's health (mode,
// crash-boot count, reset reason, net, sd, rssi, heap, uptime) even when its SD or
// app plane is wedged. bodyJson is a compact JSON snapshot. SD-independent.
inline bool halPostHeartbeat(const char* device, const char* bodyJson) {
    if (!g_hal || !g_hal->network || !device || !bodyJson) return false;
    S3Creds creds = loadS3Creds();
    if (!creds.valid) return false;
    int bodyLen = (int)strlen(bodyJson);
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
        "POST /prod/command/heartbeat?device=%s HTTP/1.1\r\n"
        "Host: %s\r\nx-api-key: %s\r\n"
        "Content-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        urlEncode(device).c_str(), creds.apiHost, creds.apiKey, bodyLen);
    TlsHandle tls = g_hal->network->connect(creds.apiHost);
    if (!tls) return false;
    bool ok = g_hal->network->write(tls, hdr, strlen(hdr)) &&
              g_hal->network->write(tls, bodyJson, (size_t)bodyLen);
    if (ok) { std::string resp = halHttpReadResponse(tls); ok = resp.find("\"ok\"") != std::string::npos; }
    g_hal->network->destroy(tls);
    return ok;
}

// Read the .meta sidecar for a .eaofh file and return first_flight.
// metaPath is the full path to the .meta file (e.g. "/sdcard/upload/0001/f.eaofh.meta").
inline uint32_t halReadMetaFirstFlight(const char* metaPath) {
    if (!g_hal || !g_hal->filesys) return 0;
    void* mf = g_hal->filesys->open(metaPath, "rb");
    if (!mf) return 0;
    char buf[32] = {};
    g_hal->filesys->read(mf, buf, sizeof(buf) - 1);
    g_hal->filesys->close(mf);
    unsigned long ff = 0, lf = 0;
    sscanf(buf, "%lu:%lu", &ff, &lf);
    return (uint32_t)ff;
}

// Scan a .eaofh file for the byte offset where flight > hwm begins.
// Uses same estimate-then-scan approach as the firmware's findSplitOffset.
// f must be open; fileSize is total bytes.
inline uint64_t halFindSplitOffset(void* f, uint64_t fileSize,
                                    uint32_t firstFlight, uint32_t lastFlight,
                                    uint32_t hwm) {
    if (!g_hal || !g_hal->filesys) return 0;
    if (firstFlight > hwm)  return 0;
    if (lastFlight  <= hwm) return fileSize;

    uint32_t total = lastFlight - firstFlight + 1;
    uint32_t avg   = (total > 0) ? (uint32_t)(fileSize / total) : 0;
    if (avg == 0) return 0;

    float frac   = (float)(hwm - firstFlight + 1) / (float)total;
    uint64_t est = (uint64_t)(frac * (float)fileSize);
    uint64_t win = (uint64_t)avg * 3;
    uint64_t scan_start = (est > win) ? (est - win) : 0;
    uint64_t scan_end   = est + win;
    if (scan_end > fileSize) scan_end = fileSize;

    g_hal->filesys->seek(f, (long)scan_start, 0 /*SEEK_SET*/);

    const uint32_t CHUNK = 4096;
    uint8_t buf[CHUNK + 4];
    uint64_t pos   = scan_start;
    uint64_t hwm_end = 0;
    uint32_t carry = 0;

    while (pos < scan_end) {
        uint32_t want = (scan_end - pos > CHUNK) ? CHUNK : (uint32_t)(scan_end - pos);
        uint32_t got = (uint32_t)g_hal->filesys->read(f, buf + carry, want);
        if (got == 0) break;
        uint32_t avail = carry + got;

        for (uint32_t i = 0; i + 3 < avail; i++) {
            if (buf[i] != 0xEA || buf[i+1] != 0x4C) continue;
            uint16_t rlen = ((uint16_t)buf[i+2] << 8) | buf[i+3];
            if (rlen < 28) continue;
            uint64_t recAbs = (pos - carry) + i;
            if (recAbs + rlen > fileSize) continue;

            // Read body[20:22] — flight number
            uint8_t fnum[2] = {};
            long saved = (long)(recAbs + 4 + 20);
            g_hal->filesys->seek(f, saved, 0);
            g_hal->filesys->read(f, fnum, 2);
            g_hal->filesys->seek(f, (long)(pos + got), 0);  // restore

            uint32_t fl = ((uint32_t)fnum[0] << 8) | fnum[1];
            if (fl == hwm) {
                hwm_end = recAbs + rlen;
            } else if (fl > hwm) {
                return hwm_end > 0 ? hwm_end : recAbs;
            }
        }

        if (avail >= 3) { carry = 3; memmove(buf, buf + avail - 3, 3); }
        else            { carry = avail; }
        pos += got;
    }
    return hwm_end;
}

// Fleet-aware upload for a single .eaofh file:
// 1. Parse filename for (serial, last_flight).
// 2. Fetch/cache manifest → skip if last_flight <= hwm.
// 3. If first_flight <= hwm, extract delta (upload tail only).
// 4. Upload to aircraft/{serial}/{barename} S3 path.
// 5. Update manifest + write DSU cookie.
// Returns: success (true) or failure (false). skip is also reported as success.
inline UploadResult halS3UploadEaofh(const char* harvestDir, const char* relPath,
                                      UploadProgressFn progress = nullptr) {
    UploadResult res = {};
    if (!g_hal) { strlcpy(res.error, "HAL null", sizeof(res.error)); return res; }

    char fullpath[256];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", harvestDir, relPath);

    // Extract bare filename (strip harvest path-flattening prefix "xxx__")
    const char* fname = strrchr(relPath, '/');
    fname = fname ? fname + 1 : relPath;
    const char* bareName = fname;
    for (const char* p = fname; p[0] && p[1]; p++)
        if (p[0] == '_' && p[1] == '_') bareName = p + 2;

    // Parse for (serial, last_flight)
    char serial[44] = "";
    uint32_t lastFlight = 0;
    if (!parseEaofhFilename(bareName, serial, sizeof(serial), &lastFlight)) {
        strlcpy(res.error, "not eaofh", sizeof(res.error));
        return res;
    }

    ULDBG("ULDBG eaofh begin %s last=%u", bareName, lastFlight);

    // Fetch manifest (in-session cache via static vars)
    static char  s_cachedSerial[44] = "";
    static uint32_t s_cachedHwm = 0;
    if (strcmp(s_cachedSerial, serial) != 0) {
        ULDBG("ULDBG manifest fetch %s...", serial);
        s_cachedHwm = halFetchManifest(serial);
        ULDBG("ULDBG manifest hwm=%u", s_cachedHwm);
        strlcpy(s_cachedSerial, serial, sizeof(s_cachedSerial));
        if (g_hal->nvs) {
            g_hal->nvs->set_str("mfst", "serial", serial);
            g_hal->nvs->set_u32("mfst", "hwm", s_cachedHwm);
        }
        printf("[S3] Manifest %s hwm=%lu\n", serial, (unsigned long)s_cachedHwm);
    }

    if (lastFlight <= s_cachedHwm) {
        // Entirely covered by S3 — skip
        printf("[S3] Skip %s (last=%lu hwm=%lu)\n", bareName,
               (unsigned long)lastFlight, (unsigned long)s_cachedHwm);
        res.success = true;
        strlcpy(res.error, "skipped (manifest hwm covers)", sizeof(res.error));
        // Remove .meta sidecar if present
        char metaPath[272];
        snprintf(metaPath, sizeof(metaPath), "%s.meta", fullpath);
        g_hal->filesys->remove(metaPath);
        return res;
    }

    // Read .meta for first_flight
    uint32_t firstFlight = 0;
    {
        char metaPath[272];
        snprintf(metaPath, sizeof(metaPath), "%s.meta", fullpath);
        firstFlight = halReadMetaFirstFlight(metaPath);
    }

    // Is the harvested file gzip-compressed? (compress directive). A gzip stream is an
    // indivisible unit — you CANNOT byte-split it: the split-delta below scans for an
    // .eaofh record boundary, but on gzip bytes it returns a garbage offset and uploading
    // only [offset..end] yields a HEADLESS, corrupt object that won't gunzip (the cause of
    // truncated/invalid flights in S3 under `compress on`). Detect the gzip magic and skip
    // the delta entirely — a compressed file is always uploaded whole.
    bool fileIsGzip = false;
    {
        void* gf = g_hal->filesys->open(fullpath, "rb");
        if (gf) {
            unsigned char magic[2] = {0, 0};
            g_hal->filesys->read(gf, magic, 2);
            g_hal->filesys->close(gf);
            fileIsGzip = (magic[0] == 0x1f && magic[1] == 0x8b);
        }
    }

    // Find split offset if file has pre-hwm content (uncompressed .eaofh only).
    uint64_t splitOffset = 0;
    if (!fileIsGzip && firstFlight > 0 && firstFlight <= s_cachedHwm) {
        uint32_t totalSz = 0; bool isDir = false;
        g_hal->filesys->stat(fullpath, &totalSz, &isDir);
        if (totalSz > 0) {
            void* f = g_hal->filesys->open(fullpath, "rb");
            if (f) {
                splitOffset = halFindSplitOffset(f, totalSz, firstFlight, lastFlight, s_cachedHwm);
                g_hal->filesys->close(f);
                if (splitOffset >= totalSz) splitOffset = 0;
            }
        }
        printf("[S3] Delta %s offset=%llu\n", bareName, (unsigned long long)splitOffset);
    } else if (fileIsGzip) {
        printf("[S3] %s is gzip — uploading whole (no delta split)\n", bareName);
    }

    // Build aircraft-namespaced S3 key
    char s3Key[256];
    snprintf(s3Key, sizeof(s3Key), "aircraft/%s/%s", serial, bareName);

    // Get file size for upload
    uint32_t totalSz = 0; bool isDir = false;
    g_hal->filesys->stat(fullpath, &totalSz, &isDir);
    uint32_t uploadSize = (splitOffset < totalSz) ? (totalSz - (uint32_t)splitOffset) : 0;
    if (uploadSize == 0) {
        res.success = true;
        strlcpy(res.error, "empty delta", sizeof(res.error));
        return res;
    }

    S3Creds creds = loadS3Creds();
    if (!creds.valid) { strlcpy(res.error, "no creds", sizeof(res.error)); return res; }

    // Presign with explicit key= and upload size
    char query[640];
    {
        std::string encKey = urlEncode(s3Key);
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s&key=%s",
                 urlEncode(bareName).c_str(), uploadSize, creds.deviceId, encKey.c_str());
    }
    ULDBG("ULDBG eaofh presign uploadSize=%u split=%llu...", uploadSize, (unsigned long long)splitOffset);
    std::string presignResp = s3ApiGetViaHal(creds.apiHost, creds.apiKey, query);
    ULDBG("ULDBG eaofh presign resp len=%u", (unsigned)presignResp.size());

    if (presignResp.find("\"skip\"") != std::string::npos &&
        presignResp.find("true") != std::string::npos) {
        // S3 already has this exact upload (idempotent retry)
        res.success = true;
        strlcpy(res.error, "skipped (S3 exists)", sizeof(res.error));
    } else {
        std::string url = jsonStr(presignResp, "url");
        std::string uploadId = jsonStr(presignResp, "upload_id");

        if (splitOffset == 0) {
            // Full file (no delta): delegate to halS3UploadFile with aircraft key override.
            // This gets full multipart support and NVS resume for free.
            ULDBG("ULDBG eaofh -> halS3UploadFile (full)");
            res = halS3UploadFile(fullpath, bareName, progress, s3Key);
        } else if (!url.empty() && uploadId.empty()) {
            // Delta upload — small enough for single PUT (< 5 MB)
            void* f = g_hal->filesys->open(fullpath, "rb");
            if (!f) { strlcpy(res.error, "open failed", sizeof(res.error)); return res; }
            g_hal->filesys->seek(f, (long)splitOffset, 0);
            char s3Host[128], s3Path[2500];
            if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
                g_hal->filesys->close(f);
                strlcpy(res.error, "URL parse failed", sizeof(res.error));
                return res;
            }
            TlsHandle tls = g_hal->network->connect(s3Host);
            if (!tls) {
                g_hal->filesys->close(f);
                strlcpy(res.error, "TLS failed", sizeof(res.error));
                return res;
            }
            uint32_t t0 = g_hal->clock->millis();
            char hdr[2700];
            snprintf(hdr, sizeof(hdr),
                "PUT %s HTTP/1.1\r\nHost: %s\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
                s3Path, s3Host, uploadSize);
            if (g_hal->network->write(tls, hdr, strlen(hdr)) &&
                halStreamFile(tls, f, uploadSize, progress)) {
                char etag[64] = ""; int status = 0;
                halHttpReadResponse(tls, etag, sizeof(etag), &status);
                if (status == 200 && etag[0]) {
                    uint32_t elapsed = g_hal->clock->millis() - t0;
                    res.success = true;
                    res.kbps = elapsed > 0 ? uploadSize / 1024.0f / (elapsed / 1000.0f) : 0;
                } else {
                    // Same guard as the single-PUT path: only a confirmed 200+ETag counts,
                    // else we'd advance the manifest over a truncated delta object.
                    snprintf(res.error, sizeof(res.error), "delta PUT not confirmed (HTTP %d)", status);
                }
            } else {
                strlcpy(res.error, "stream failed", sizeof(res.error));
            }
            g_hal->network->destroy(tls);
            g_hal->filesys->close(f);
        } else {
            // Large delta (> 5 MB) needs multipart — uncommon; fall through to
            // full halS3UploadFile which doesn't support offset, so upload from 0
            // (some redundant data sent but the result is correct).
            printf("[S3] Large delta — falling back to full upload for %s\n", bareName);
            res = halS3UploadFile(fullpath, bareName, progress, s3Key);
        }
    }

    if (res.success) {
        // Update manifest + cookie
        uint32_t newFirst = (firstFlight > 0 && firstFlight <= s_cachedHwm)
                            ? (s_cachedHwm + 1) : (firstFlight > 0 ? firstFlight : 1);
        if (halUpdateManifest(serial, newFirst, lastFlight, s3Key)) {
            s_cachedHwm = lastFlight;
            if (g_hal->nvs) g_hal->nvs->set_u32("mfst", "hwm", s_cachedHwm);
        }
        // Remove .meta sidecar
        char metaPath[272];
        snprintf(metaPath, sizeof(metaPath), "%s.meta", fullpath);
        g_hal->filesys->remove(metaPath);
    }
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

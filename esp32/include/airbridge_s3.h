#pragma once
// AirBridge S3 multipart upload session management
// Extracted from s3UploadFile() for testing NVS resume/retry logic.

#include "hal/hal.h"
#include "airbridge_utils.h"
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

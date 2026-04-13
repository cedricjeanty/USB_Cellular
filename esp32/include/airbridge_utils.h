#pragma once
// AirBridge — pure utility functions (no ESP-IDF / FreeRTOS dependencies)
// Extracted from main.cpp for native unit testing.

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstdint>
#include <string>

// strlcpy is provided by ESP-IDF's newlib but absent from glibc on Linux.
#if !defined(ESP_PLATFORM)
#ifndef strlcpy
inline size_t strlcpy(char* dst, const char* src, size_t sz) {
    size_t srclen = strlen(src);
    if (sz > 0) {
        size_t cplen = (srclen >= sz) ? sz - 1 : srclen;
        memcpy(dst, src, cplen);
        dst[cplen] = '\0';
    }
    return srclen;
}
#endif
#endif

// ── Version comparison ──────────────────────────────────────────────────────
inline bool versionNewer(const char* available, const char* current) {
    // YYYYMMDDHHMMSS format (14+ digits) — compare as integer
    if (strlen(available) >= 14 && strlen(current) >= 14 &&
        available[0] >= '0' && available[0] <= '9' &&
        current[0] >= '0' && current[0] <= '9') {
        return strtoull(available, nullptr, 10) > strtoull(current, nullptr, 10);
    }
    // Legacy X.Y.Z semver fallback
    int av[3] = {0,0,0}, cv[3] = {0,0,0};
    sscanf(available, "%d.%d.%d", &av[0], &av[1], &av[2]);
    sscanf(current,   "%d.%d.%d", &cv[0], &cv[1], &cv[2]);
    for (int i = 0; i < 3; i++) {
        if (av[i] > cv[i]) return true;
        if (av[i] < cv[i]) return false;
    }
    return false;
}

// ── Lightweight JSON helpers (no library dependency) ────────────────────────
inline std::string jsonStr(const std::string& json, const char* key) {
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return "";
    // Skip whitespace after colon
    size_t valStart = colon + 1;
    while (valStart < json.size() && json[valStart] == ' ') valStart++;
    // Check for null before looking for quotes
    if (json.compare(valStart, 4, "null") == 0) return "";
    size_t q1 = json.find('"', valStart);
    if (q1 == std::string::npos) return "";
    // Make sure the quote is the value, not the next key (no comma between colon and quote)
    for (size_t i = valStart; i < q1; i++) {
        if (json[i] == ',' || json[i] == '}') return "";
    }
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

inline int jsonInt(const std::string& json, const char* key) {
    size_t pos = json.find(key);
    if (pos == std::string::npos) return -1;
    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return -1;
    return atoi(json.c_str() + colon + 1);
}

// ── URL encoding / decoding ─────────────────────────────────────────────────
inline std::string urlEncode(const char* s) {
    std::string out;
    for (; *s; s++) {
        if (isalnum((unsigned char)*s) || *s == '-' || *s == '_' || *s == '.' || *s == '~') {
            out += *s;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)*s);
            out += hex;
        }
    }
    return out;
}

inline std::string url_decode(const char* src, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            char hex[3] = {src[i+1], src[i+2], 0};
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (src[i] == '+') {
            out += ' ';
        } else {
            out += src[i];
        }
    }
    return out;
}

// ── Form field extraction ───────────────────────────────────────────────────
inline std::string form_field(const char* body, const char* name) {
    std::string needle = std::string(name) + "=";
    const char* start = strstr(body, needle.c_str());
    if (!start) return "";
    start += needle.length();
    const char* end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    return url_decode(start, len);
}

// ── URL parsing ─────────────────────────────────────────────────────────────
inline bool parseUrl(const std::string& url, char* host, size_t hostSz,
                     char* path, size_t pathSz) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        strlcpy(host, url.substr(hostStart).c_str(), hostSz);
        strlcpy(path, "/", pathSz);
    } else {
        strlcpy(host, url.substr(hostStart, pathStart - hostStart).c_str(), hostSz);
        strlcpy(path, url.substr(pathStart).c_str(), pathSz);
    }
    return true;
}

// ── RSSI to signal bars ─────────────────────────────────────────────────────
inline int8_t rssiToBars(int32_t rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -80) return 2;
    if (rssi >= -90) return 1;
    return 0;
}

// ── File skip list ──────────────────────────────────────────────────────────
namespace {
const char* const SKIP_NAMES[] = {
    "System Volume Information", "desktop.ini", "Thumbs.db",
    ".Spotlight-V100", ".Trashes", ".fseventsd", "harvested",
    "airbridge.log", "dsuCookie.easdf",
    "firmware.bin", "_firmware.bin", "ENABLE_CDC", "ENABLE_MSC",
    nullptr
};
}

inline bool isSkipped(const char* n) {
    if (n[0] == '.' || n[0] == '~') return true;
    for (int i = 0; SKIP_NAMES[i]; i++)
        if (strcmp(n, SKIP_NAMES[i]) == 0) return true;
    return false;
}

// ── Size formatting ─────────────────────────────────────────────────────────
// Full format with "B" suffix (used in CLI/logs)
inline void _fmtSize(char* buf, size_t len, float mb) {
    if (mb >= 1000.0f)   snprintf(buf, len, "%.1fGB", mb / 1024.0f);
    else if (mb >= 0.1f) snprintf(buf, len, "%.1fMB", mb);
    else                 snprintf(buf, len, "%.1fKB", mb * 1024.0f);
}

// Short format without "B" — for display (fits size 2 font in 62px column)
// K, M, G: max "999M" = 4 chars
inline void _fmtSizeShort(char* buf, size_t len, float mb) {
    if (mb >= 1000.0f)       snprintf(buf, len, "%.1fG", mb / 1024.0f);
    else if (mb >= 100.0f)   snprintf(buf, len, "%.0fM", mb);
    else if (mb >= 0.1f)     snprintf(buf, len, "%.1fM", mb);
    else if (mb * 1024 >= 100.0f) snprintf(buf, len, "%.0fK", mb * 1024.0f);
    else                     snprintf(buf, len, "%.1fK", mb * 1024.0f);
}

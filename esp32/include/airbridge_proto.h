#pragma once
// AirBridge — protocol & binary helpers (no ESP-IDF / FreeRTOS dependencies)
// Extracted from main.cpp for native unit testing.

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <string>
#include "airbridge_utils.h"  // for strlcpy shim

// ── HTTP chunked transfer decoding ──────────────────────────────────────────
inline std::string dechunk(const std::string& raw) {
    std::string body;
    size_t pos = 0;
    while (pos < raw.length()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string szLine = raw.substr(pos, nl - pos);
        while (!szLine.empty() && (szLine.back() == '\r' || szLine.back() == ' '))
            szLine.pop_back();
        unsigned long chunkSz = strtoul(szLine.c_str(), nullptr, 16);
        if (chunkSz == 0) break;
        size_t dataStart = nl + 1;
        if (dataStart + chunkSz <= raw.length())
            body.append(raw, dataStart, chunkSz);
        pos = dataStart + chunkSz + 2;  // +2 for \r\n after chunk data
    }
    return body;
}

// ── CRC-16 (poly 0x8005, init 0xFFFF) ──────────────────────────────────────
inline uint16_t crc16_8005(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x8005;
            else crc <<= 1;
            crc &= 0xFFFF;
        }
    }
    return crc;
}

// ── DSU cookie binary builder (78 bytes) ────────────────────────────────────
inline void buildDsuCookie(const char* serial, uint32_t flightNum, uint8_t* out) {
    memset(out, 0, 78);
    out[0] = 0xEA;   // magic
    out[1] = 0x1E;   // file type
    out[2] = 0x00; out[3] = 78;  // length BE u16
    out[4] = 0xD1;   // DSU hardware ID
    // Serial at offset 9 (43 bytes, null-padded)
    strlcpy((char*)&out[9], serial, 43);
    // Mode flag
    out[60] = 0x01;
    // Flight number at offset 62 (BE u32)
    out[62] = (flightNum >> 24) & 0xFF;
    out[63] = (flightNum >> 16) & 0xFF;
    out[64] = (flightNum >> 8) & 0xFF;
    out[65] = flightNum & 0xFF;
    // CRC-16 over bytes 0-75
    uint16_t crc = crc16_8005(out, 76);
    out[76] = (crc >> 8) & 0xFF;
    out[77] = crc & 0xFF;
}

// ── DSU .eaofh filename parser ──────────────────────────────────────────────
// Parses filenames like "EA500.000243_01218_20260406.eaofh"
// or "flightHistory__EA500.000243_01218_20260406.eaofh"
// Returns serial (e.g. "EA500.000243") and flight number (e.g. 1218).
inline bool parseDsuFilename(const char* name, char* serial, size_t serialSz, uint32_t* flightNum) {
    const char* p = strstr(name, "EA");
    if (!p) return false;
    const char* us = strchr(p, '_');
    if (!us || (size_t)(us - p) >= serialSz) return false;
    memcpy(serial, p, us - p);
    serial[us - p] = '\0';
    *flightNum = strtoul(us + 1, nullptr, 10);
    return true;
}

// ── Harvest path flattening ─────────────────────────────────────────────────
// Converts nested directory paths to flat names with __ separator.
// e.g. prefix="logs", name="data.bin" → "logs__data.bin"
inline void flattenPath(const char* prefix, const char* name, char* out, size_t outSz) {
    if (prefix[0])
        snprintf(out, outSz, "%s__%s", prefix, name);
    else
        strlcpy(out, name, outSz);
}

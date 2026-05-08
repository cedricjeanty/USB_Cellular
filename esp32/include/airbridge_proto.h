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

// ── ARINC-429 bit/BCD helpers (for date-mode cookies) ──────────────────────
inline uint8_t reverse_bits_8(uint8_t v) {
    uint8_t r = 0;
    for (int i = 0; i < 8; i++) r = (r << 1) | ((v >> i) & 1);
    return r;
}
inline uint8_t to_bcd(uint8_t value) {  // 0..99 → packed BCD
    return ((value / 10) << 4) | (value % 10);
}
// Encode date into 3 ARINC data bytes used after the 0x90 label.
// Inverse of the BCD/bit-reversal decode in EA500's parser; matches
// ea500/cookie.py:_encode_arinc_date.
inline void encode_arinc_date(uint16_t year, uint8_t month, uint8_t day,
                              uint8_t* b2, uint8_t* b3, uint8_t* b4) {
    uint8_t year_bcd  = to_bcd((uint8_t)(year - 2000));
    uint8_t month_bcd = to_bcd(month);
    uint8_t day_bcd   = to_bcd(day);

    uint8_t b2r = (year_bcd << 2) & 0xFF;
    uint8_t b3r = (uint8_t)(((day_bcd & 1) << 7) | ((month_bcd << 2) & 0x7F));
    uint8_t b4r = (uint8_t)((day_bcd >> 1) & 0x1F);

    uint8_t bb2 = reverse_bits_8(b2r);
    uint8_t bb3 = reverse_bits_8(b3r);
    uint8_t bb4 = reverse_bits_8(b4r);

    // ARINC-429 odd parity: bit 0 of b4
    int ones = __builtin_popcount(0x90) + __builtin_popcount(bb2)
             + __builtin_popcount(bb3) + __builtin_popcount(bb4);
    if ((ones & 1) == 0) bb4 |= 0x01;

    *b2 = bb2; *b3 = bb3; *b4 = bb4;
}

// ── DSU cookie binary builder (78 bytes) ────────────────────────────────────
// Shared header layout for both modes.
inline void _buildDsuCookieHeader(const char* serial, uint8_t* out) {
    memset(out, 0, 78);
    out[0] = 0xEA;   // magic
    out[1] = 0x1E;   // file type
    out[2] = 0x00; out[3] = 78;  // length BE u16
    out[4] = 0xD1;   // DSU hardware ID
    strlcpy((char*)&out[9], serial, 43);
    out[60] = 0x01;  // mode flag
}
inline void _finalizeDsuCookie(uint8_t* out) {
    uint16_t crc = crc16_8005(out, 76);
    out[76] = (crc >> 8) & 0xFF;
    out[77] = crc & 0xFF;
}

// Flight-number mode: download flights after this number.
inline void buildDsuCookie(const char* serial, uint32_t flightNum, uint8_t* out) {
    _buildDsuCookieHeader(serial, out);
    out[62] = (flightNum >> 24) & 0xFF;
    out[63] = (flightNum >> 16) & 0xFF;
    out[64] = (flightNum >> 8) & 0xFF;
    out[65] = flightNum & 0xFF;
    _finalizeDsuCookie(out);
}

// Date mode: download flights from this date onward.
// ARINC date word at offset 52 (0x90 label + 3 BCD bytes), end sentinel
// 0xFFFFFFFF at offset 62. Matches EA500's encode_cookie(target_date=...).
inline void buildDsuCookieDate(const char* serial, uint16_t year,
                               uint8_t month, uint8_t day, uint8_t* out) {
    _buildDsuCookieHeader(serial, out);
    uint8_t b2, b3, b4;
    encode_arinc_date(year, month, day, &b2, &b3, &b4);
    out[52] = 0x90;
    out[53] = b2;
    out[54] = b3;
    out[55] = b4;
    out[62] = 0xFF; out[63] = 0xFF; out[64] = 0xFF; out[65] = 0xFF;
    _finalizeDsuCookie(out);
}

// ── DSU .eaofh log file content parser ──────────────────────────────────────
// .eaofh files are append-only sequences of records framed as:
//   [0xEA] [type] [BE16 length] [body...]
// Body offsets used here (same for 0x0E, 0x4B, 0x4C):
//   body[5:17]  — serial (12 bytes ASCII, NUL-padded right)
//   body[20:22] — flight number (BE u16)
// The last flight is found by backward-scanning from EOF for the last valid
// 0x0E or 0x4C record (TOC / per-flight summary). 0x4B (date word) is also
// readable at the same offsets but we stick to 0x0E/0x4C.
//
// Reader callback: read len bytes at absolute offset; return bytes read.
// Tests use a stdio adapter; firmware uses a HAL filesys adapter.
typedef uint32_t (*log_read_at_fn)(void* ctx, uint64_t off, uint8_t* buf, uint32_t len);

inline bool lastRecordFromLog(log_read_at_fn readfn, void* ctx, uint64_t fsize,
                              uint32_t* outFlight, char* outSerial, size_t serialSz) {
    if (!readfn || !outFlight || !outSerial || serialSz < 13 || fsize < 28) return false;
    *outFlight = 0;
    outSerial[0] = '\0';

    const uint32_t CHUNK   = 1024;
    const uint32_t OVERLAP = 4;
    uint8_t buf[CHUNK + OVERLAP];

    uint64_t pos = fsize;
    while (pos > 0) {
        uint32_t want = (pos > CHUNK) ? CHUNK : (uint32_t)pos;
        pos -= want;
        bool atTail = (pos + want < fsize);
        uint32_t toRead = want + (atTail ? OVERLAP : 0);
        uint32_t got = readfn(ctx, pos, buf, toRead);
        if (got < want) return false;

        for (int32_t i = (int32_t)want - 1; i >= 0; --i) {
            if (buf[i] != 0xEA) continue;
            if ((uint32_t)i + 4 > got) continue;
            uint8_t rt = buf[i + 1];
            if (rt != 0x0E && rt != 0x4C) continue;
            uint16_t rlen = ((uint16_t)buf[i + 2] << 8) | buf[i + 3];
            if (rlen < 24 || rlen > 4096) continue;
            uint64_t recOff = pos + (uint32_t)i;
            if (recOff + rlen > fsize) continue;
            // Validate framing: byte at recOff+rlen must be 0xEA, or be EOF.
            if (recOff + rlen < fsize) {
                uint8_t nextSync = 0;
                if (readfn(ctx, recOff + rlen, &nextSync, 1) != 1) continue;
                if (nextSync != 0xEA) continue;
            }
            // Extract serial (body[5:17]) and flight (body[20:22]).
            uint8_t hdr[24];
            if (readfn(ctx, recOff + 4, hdr, 24) != 24) continue;
            // Serial: 12 bytes ASCII, NUL-padded right; also strip trailing space/non-print.
            size_t copyLen = (serialSz - 1 < 12) ? (serialSz - 1) : 12;
            memcpy(outSerial, hdr + 5, copyLen);
            outSerial[copyLen] = '\0';
            for (int j = (int)copyLen - 1; j >= 0; --j) {
                uint8_t c = (uint8_t)outSerial[j];
                if (c == 0 || c == ' ' || c < 0x20 || c > 0x7E) outSerial[j] = '\0';
                else break;
            }
            *outFlight = ((uint32_t)hdr[20] << 8) | hdr[21];
            return true;
        }
        // Slide overlap to handle a sync straddling chunk boundary.
        if (pos > 0) pos += OVERLAP;
    }
    return false;
}

// Stdio adapter for native tests / fopen-on-SD on ESP32.
inline uint32_t stdio_read_at(void* ctx, uint64_t off, uint8_t* buf, uint32_t len) {
    FILE* f = (FILE*)ctx;
    if (fseek(f, (long)off, SEEK_SET) != 0) return 0;
    return (uint32_t)fread(buf, 1, len, f);
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

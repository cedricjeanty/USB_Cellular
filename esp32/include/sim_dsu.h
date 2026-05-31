#pragma once
// Aircraft DSU (Data Storage Unit) Simulator
// Simulates the real EA500 DSU: reads dsuCookie.easdf, downloads only the
// flights it hasn't sent yet (blockStart..blockEnd from the internal memory
// file), and writes the slice to flightHistory/ exactly like the real unit.
//
// internalMemoryPath MUST be set before calling runSession().
// There is no synthetic data fallback.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#include "airbridge_proto.h"  // buildDsuCookie, lastRecordFromLog, crc16_8005

// One 0x4C-delimited flight in the .eaofh internal memory file.
struct FlightEntry {
    uint32_t flight;      // flight number (BE u16 from 0x4C body[20:22])
    uint64_t blockStart;  // first byte of this flight's block (inclusive)
    uint64_t blockEnd;    // first byte of next flight's block (exclusive)
    char serial[13];      // serial from 0x4C body[5:17], NUL-terminated
};

// Forward-scan a .eaofh file for 0x4C records to build a flight index.
// Each entry spans [blockStart, blockEnd) — the byte range that belongs to
// one flight. Blocks are contiguous and cover the entire file.
inline std::vector<FlightEntry> buildFlightIndex(const char* path) {
    std::vector<FlightEntry> index;
    if (!path) return index;

    FILE* f = fopen(path, "rb");
    if (!f) { printf("[DSU] Cannot open %s for indexing\n", path); return index; }

    fseek(f, 0, SEEK_END);
    uint64_t filesize = (uint64_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    const uint32_t BUFSIZE = 512 * 1024;
    std::vector<uint8_t> buf(BUFSIZE);
    uint64_t bufBase = 0;   // file offset of buf[0]
    uint32_t bufFill = 0;   // valid bytes in buf
    uint64_t offset = 0;    // current parse position (absolute)
    uint64_t blockStart = 0;

    // Load chunk from file at given position
    auto refill = [&](uint64_t from) {
        fseek(f, (long)from, SEEK_SET);
        bufFill = (uint32_t)fread(buf.data(), 1, BUFSIZE, f);
        bufBase = from;
    };
    refill(0);

    while (offset < filesize) {
        uint32_t pos = (uint32_t)(offset - bufBase);

        // Ensure header bytes are in buffer
        if (pos + 4 > bufFill) {
            if (offset >= bufBase + bufFill) {
                refill(offset);
                if (bufFill < 4) break;
                pos = 0;
            } else {
                // < 4 bytes remain in buffer — refill from current offset
                refill(offset);
                if (bufFill < 4) break;
                pos = 0;
            }
        }

        if (buf[pos] != 0xEA) { offset++; continue; }

        uint8_t type = buf[pos + 1];
        uint16_t recLen = ((uint16_t)buf[pos + 2] << 8) | buf[pos + 3];

        if (recLen < 4) { offset++; continue; }
        if (offset + recLen > filesize) break;  // truncated at EOF

        if (type == 0x4C && recLen >= 28) {
            // Extract body[5:17] (serial) and body[20:22] (flight BE u16).
            // Body starts at offset+4; we need bytes 5 through 21 (17 bytes).
            uint8_t body[22] = {};
            uint32_t bodyPos = pos + 4;

            if (bodyPos + 22 <= bufFill) {
                memcpy(body, buf.data() + bodyPos, 22);
            } else {
                // Body straddles chunk boundary — targeted read, then re-sync
                fseek(f, (long)(offset + 4), SEEK_SET);
                fread(body, 1, 22, f);
                refill(offset + recLen);
            }

            uint32_t flight = ((uint32_t)body[20] << 8) | body[21];
            if (flight > 0 && flight < 65535) {
                FlightEntry e;
                e.flight = flight;
                e.blockStart = blockStart;
                e.blockEnd = offset + recLen;
                memcpy(e.serial, body + 5, 12);
                e.serial[12] = '\0';
                for (int j = 11; j >= 0; j--) {
                    if (e.serial[j] == '\0' || e.serial[j] == ' ') e.serial[j] = '\0';
                    else break;
                }
                index.push_back(e);
                blockStart = offset + recLen;
            }
        }

        offset += recLen;

        // Refill buffer if we've advanced past it
        if (offset >= bufBase + bufFill && offset < filesize) {
            refill(offset);
        }
    }

    // The tail after the last 0x4C belongs to the last flight's block.
    if (!index.empty()) index.back().blockEnd = filesize;

    fclose(f);
    return index;
}

class SimDSU {
public:
    const char* serial = "EA500.000243";
    const char* sdRoot = "./emu_sdcard";
    int writeSpeedKBps = 500;       // throttle for copyFileSlice (~500 KB/s observed on real device)
    // Path to directory containing real metric files to copy verbatim.
    // If null, fallback writes zeroed files of correct size.
    const char* metricsSource = nullptr;

    // REQUIRED: path to real .eaofh file representing DSU internal memory.
    // runSession() returns failure immediately if this is null.
    const char* internalMemoryPath = nullptr;

    // Upper bound on flights to include in this session (UINT32_MAX = all).
    uint32_t maxFlight = UINT32_MAX;

    struct SessionResult {
        bool success;
        char filename[128];   // e.g. "EA500.000243_01077_20250301.eaofh"
        uint32_t firstFlight; // first flight in downloaded slice (0 = nothing)
        uint32_t flightNum;   // last flight in downloaded slice (0 = nothing)
        uint32_t bytesWritten;
    };

    // Read and validate the cookie from sdRoot/dsuCookie.easdf.
    // Returns the flight number, or 0 if absent, corrupt CRC, or date-mode.
    uint32_t readCookie() {
        char path[256];
        snprintf(path, sizeof(path), "%s/dsuCookie.easdf", sdRoot);
        FILE* f = fopen(path, "rb");
        if (!f) return 0;
        uint8_t cookie[78];
        size_t n = fread(cookie, 1, 78, f);
        fclose(f);
        if (n < 78) return 0;
        if (cookie[0] != 0xEA || cookie[1] != 0x1E) return 0;

        uint16_t computed = crc16_8005(cookie, 76);
        uint16_t stored = ((uint16_t)cookie[76] << 8) | cookie[77];
        if (computed != stored) {
            printf("[DSU] Cookie CRC mismatch (0x%04X vs 0x%04X)\n", computed, stored);
            return 0;
        }

        uint32_t flight = ((uint32_t)cookie[62] << 24) | ((uint32_t)cookie[63] << 16) |
                          ((uint32_t)cookie[64] << 8) | cookie[65];
        if (flight == 0xFFFFFFFF) {
            printf("[DSU] Date-mode cookie — treating as no cookie\n");
            return 0;
        }
        return flight;
    }

    // Run a DSU download session using internalMemoryPath as the source.
    // Reads the cookie, finds new flights (after cookieFlight, up to maxFlight),
    // copies the byte slice to flightHistory/, writes metrics + report.
    // Returns success=true with bytesWritten=0 if already caught up.
    SessionResult runSession() {
        SessionResult r = {};
        if (!internalMemoryPath) {
            printf("[DSU] ERROR: internalMemoryPath not set\n");
            return r;
        }

        uint32_t cookieFlight = readCookie();
        if (cookieFlight > 0)
            printf("[DSU] Cookie: last downloaded flight %u\n", cookieFlight);
        else
            printf("[DSU] No cookie — downloading from beginning\n");

        loadIndex();
        if (s_index.empty()) {
            printf("[DSU] ERROR: empty flight index from %s\n", internalMemoryPath);
            return r;
        }

        // First entry with flight > cookieFlight
        auto startIt = std::upper_bound(s_index.begin(), s_index.end(), cookieFlight,
            [](uint32_t val, const FlightEntry& e){ return val < e.flight; });

        if (startIt == s_index.end()) {
            printf("[DSU] Caught up — no flights after %u\n", cookieFlight);
            r.success = true;
            return r;
        }

        // Last entry with flight <= maxFlight
        auto endIt = s_index.end();
        if (maxFlight != UINT32_MAX) {
            endIt = std::upper_bound(s_index.begin(), s_index.end(), maxFlight,
                [](uint32_t val, const FlightEntry& e){ return val < e.flight; });
        }
        if (startIt == endIt) {
            printf("[DSU] No flights in range (%u, %u]\n", cookieFlight, maxFlight);
            r.success = true;
            return r;
        }

        auto lastIt = std::prev(endIt);
        uint64_t sliceStart = startIt->blockStart;
        uint64_t sliceEnd = lastIt->blockEnd;
        r.firstFlight = startIt->flight;
        r.flightNum = lastIt->flight;

        const char* usedSerial = (lastIt->serial[0] != '\0') ? lastIt->serial : serial;

        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        char dateStr[16];
        snprintf(dateStr, sizeof(dateStr), "%04d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

        snprintf(r.filename, sizeof(r.filename), "%s_%05u_%s.eaofh",
                 usedSerial, r.flightNum, dateStr);

        char dir[256];
        snprintf(dir, sizeof(dir), "%s/flightHistory", sdRoot);
        ::mkdir(dir, 0755);
        snprintf(dir, sizeof(dir), "%s/metrics", sdRoot);
        ::mkdir(dir, 0755);

        char fhPath[384];
        snprintf(fhPath, sizeof(fhPath), "%s/flightHistory/%s", sdRoot, r.filename);

        r.bytesWritten = copyFileSlice(internalMemoryPath, fhPath, sliceStart, sliceEnd);
        if (r.bytesWritten == 0) {
            printf("[DSU] Failed to copy slice to %s\n", fhPath);
            return r;
        }

        printf("[DSU] %s: flights %u-%u (%u bytes)\n",
               r.filename, r.firstFlight, r.flightNum, r.bytesWritten);

        writeMetrics();
        writeReport(r.filename, r.bytesWritten, dateStr);
        r.success = true;
        return r;
    }

private:
    std::vector<FlightEntry> s_index;
    bool s_indexLoaded = false;

    void loadIndex() {
        if (s_indexLoaded) return;
        s_index = buildFlightIndex(internalMemoryPath);
        s_indexLoaded = true;
        if (s_index.empty()) return;
        printf("[DSU] Index: %zu flights (%u-%u) from %s\n",
               s_index.size(), s_index.front().flight, s_index.back().flight,
               internalMemoryPath);
    }

    uint32_t copyFileSlice(const char* src, const char* dst,
                           uint64_t start, uint64_t end) {
        if (start >= end) return 0;
        FILE* sf = fopen(src, "rb");
        if (!sf) return 0;
        FILE* df = fopen(dst, "wb");
        if (!df) { fclose(sf); return 0; }

        fseek(sf, (long)start, SEEK_SET);

        uint8_t buf[65536];
        uint64_t rem = end - start;
        uint32_t total = 0;

        while (rem > 0) {
            uint32_t chunk = (rem > sizeof(buf)) ? (uint32_t)sizeof(buf) : (uint32_t)rem;
            size_t got = fread(buf, 1, chunk, sf);
            if (got == 0) break;
            fwrite(buf, 1, got, df);
            fflush(df);
            rem -= got;
            total += (uint32_t)got;
            if (writeSpeedKBps > 0)
                usleep((uint32_t)(got * 1000 / writeSpeedKBps));
        }

        fclose(sf);
        fclose(df);
        return total;
    }

    void copyEntireFile(const char* src, const char* dst) {
        FILE* sf = fopen(src, "rb");
        if (!sf) return;
        FILE* df = fopen(dst, "wb");
        if (!df) { fclose(sf); return; }
        uint8_t buf[65536];
        size_t got;
        while ((got = fread(buf, 1, sizeof(buf), sf)) > 0)
            fwrite(buf, 1, got, df);
        fclose(sf);
        fclose(df);
    }

    // Write one metric file, copying from metricsSource if available,
    // otherwise creating a zeroed file of fallbackBytes (0 = empty file).
    void writeMetricFile(const char* name, size_t fallbackBytes) {
        char dst[384];
        snprintf(dst, sizeof(dst), "%s/metrics/%s", sdRoot, name);
        if (metricsSource) {
            char src[384];
            snprintf(src, sizeof(src), "%s/%s", metricsSource, name);
            copyEntireFile(src, dst);
            return;
        }
        FILE* f = fopen(dst, "wb");
        if (!f) return;
        if (fallbackBytes > 0) {
            std::vector<uint8_t> buf(fallbackBytes, 0);
            fwrite(buf.data(), 1, fallbackBytes, f);
        }
        fclose(f);
    }

    void writeMetrics() {
        // Real DSU writes: dsuMetric.eacmf + dsuMetric.{1,2,3,5,6,7,8}.eacmf + dsuUsage.eacuf
        // No file 4; file 2 is always 0 bytes.
        writeMetricFile("dsuMetric.eacmf", 20215);
        const int nums[] = {1, 2, 3, 5, 6, 7, 8};
        for (int n : nums) {
            char name[32];
            snprintf(name, sizeof(name), "dsuMetric.%d.eacmf", n);
            writeMetricFile(name, n == 2 ? 0 : 20215);
        }
        writeMetricFile("dsuUsage.eacuf", 640);
    }

    void writeReport(const char* filename, uint32_t bytes, const char* dateStr) {
        char path[384];
        snprintf(path, sizeof(path), "%s/downloadReport.txt", sdRoot);
        FILE* f = fopen(path, "w");
        if (!f) return;

        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        uint32_t durationSec = (writeSpeedKBps > 0) ? (bytes / 1024 / writeSpeedKBps) : 0;

        // Format matches real DSU output (GPS date + wall-clock time, KB estimate before transfer)
        fprintf(f, "Gps date(yyyymmdd) and time(HHmmss) : %s %02d%02d%02d\n",
                dateStr, tm.tm_hour, tm.tm_min, tm.tm_sec);
        fprintf(f, "Downloading usage & metric to /mnt/memStick/metrics/ ...\n");
        fprintf(f, "Finished download usage & metric to /mnt/memStick/metrics/\n");
        fprintf(f, "Downloading FH to /mnt/memStick/flightHistory/%s ...\n", filename);
        fprintf(f, "  Estimated download file size is ~%u KB\n", bytes / 1024);
        fprintf(f, "Finished download FH to /mnt/memStick/flightHistory/%s\n", filename);
        fprintf(f, "Download Report:\n");
        fprintf(f, "  Size : %u bytes, ~%u KB\n", bytes, bytes / 1024);
        fprintf(f, "  Duration : %u sec or ~%u min\n", durationSec, durationSec / 60);
        fclose(f);
    }
};

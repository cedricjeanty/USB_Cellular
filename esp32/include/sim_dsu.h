#pragma once
// Aircraft DSU (Data Storage Unit) Simulator
// Writes flight logs to the virtual SD card exactly like the real EA500 DSU.
// Reads dsuCookie.easdf to know which flight to resume from.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>

#include "airbridge_proto.h"  // for buildDsuCookie, parseDsuFilename

class SimDSU {
public:
    const char* serial = "EA500.000243";
    const char* sdRoot = "./emu_sdcard";
    uint32_t nextFlight = 1001;  // default starting flight
    int flightIncrement = 5;     // flights between sessions
    int writeSpeedKBps = 200;    // USB→SPI→SD write speed (~200 KB/s observed on real device)

    // Metrics template directory (real data from ~/EA500/)
    const char* metricsSource = nullptr;

    struct SessionResult {
        bool success;
        char filename[128];     // e.g. "EA500.000243_01001_20260411.eaofh"
        uint32_t flightNum;
        uint32_t bytesWritten;
    };

    // Read the cookie from the SD card and extract the flight number.
    // Returns 0 if no cookie or parse error.
    uint32_t readCookie() {
        char cookiePath[256];
        snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", sdRoot);

        FILE* f = fopen(cookiePath, "rb");
        if (!f) return 0;

        uint8_t cookie[78];
        size_t n = fread(cookie, 1, 78, f);
        fclose(f);
        if (n < 78) return 0;

        // Verify magic
        if (cookie[0] != 0xEA || cookie[1] != 0x1E) return 0;

        // Extract flight number (big-endian u32 at offset 62)
        uint32_t flight = ((uint32_t)cookie[62] << 24) | ((uint32_t)cookie[63] << 16) |
                          ((uint32_t)cookie[64] << 8) | cookie[65];
        return flight;
    }

    // Run a DSU download session.
    // sizeMb: size of the flight history file (0 = use sourceFile if provided)
    // sourceFile: path to a real .eaofh file to copy (nullptr = generate random data)
    SessionResult runSession(float sizeMb = 10.0f, const char* sourceFile = nullptr) {
        SessionResult r = {};

        // Read cookie to determine starting flight
        uint32_t cookieFlight = readCookie();
        if (cookieFlight > 0) {
            nextFlight = cookieFlight + flightIncrement;
            printf("[DSU] Cookie found: flight %u → starting at %u\n", cookieFlight, nextFlight);
        } else {
            printf("[DSU] No cookie — starting at flight %u\n", nextFlight);
        }
        r.flightNum = nextFlight;

        // Get today's date
        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);
        char dateStr[16];
        snprintf(dateStr, sizeof(dateStr), "%04d%02d%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

        // Build filename
        snprintf(r.filename, sizeof(r.filename), "%s_%05u_%s.eaofh",
                 serial, nextFlight, dateStr);

        // Create directories
        char dir[256];
        snprintf(dir, sizeof(dir), "%s/flightHistory", sdRoot);
        ::mkdir(dir, 0755);
        snprintf(dir, sizeof(dir), "%s/metrics", sdRoot);
        ::mkdir(dir, 0755);

        // Write flight history file
        char fhPath[384];
        snprintf(fhPath, sizeof(fhPath), "%s/flightHistory/%s", sdRoot, r.filename);

        if (sourceFile) {
            // Copy from real data file
            r.bytesWritten = copyFile(sourceFile, fhPath);
        } else {
            // Generate random data
            uint32_t targetBytes = (uint32_t)(sizeMb * 1024 * 1024);
            r.bytesWritten = generateFile(fhPath, targetBytes);
        }

        if (r.bytesWritten == 0) {
            printf("[DSU] Failed to write %s\n", fhPath);
            return r;
        }

        printf("[DSU] Wrote %s (%u bytes, %.1f MB)\n",
               r.filename, r.bytesWritten, r.bytesWritten / 1e6f);

        // Write metrics files (overwritten each session)
        writeMetrics();

        // Write download report
        writeReport(r.filename, r.bytesWritten, dateStr);

        // Remove old cookie (DSU doesn't remove it, but the AirBridge firmware
        // writes a new one after harvest)
        r.success = true;
        nextFlight += flightIncrement;
        return r;
    }

private:
    uint32_t copyFile(const char* src, const char* dst) {
        FILE* sf = fopen(src, "rb");
        if (!sf) return 0;
        FILE* df = fopen(dst, "wb");
        if (!df) { fclose(sf); return 0; }

        uint8_t buf[8192];
        uint32_t total = 0;
        while (true) {
            size_t n = fread(buf, 1, sizeof(buf), sf);
            if (n == 0) break;
            fwrite(buf, 1, n, df);
            fflush(df);
            total += n;
            if (writeSpeedKBps > 0) {
                usleep(n * 1000 / writeSpeedKBps);
            }
        }
        fclose(sf);
        fclose(df);
        return total;
    }

    uint32_t generateFile(const char* path, uint32_t size) {
        FILE* f = fopen(path, "wb");
        if (!f) return 0;

        uint8_t buf[8192];
        uint32_t rem = size;
        uint32_t seed = (uint32_t)time(nullptr);
        while (rem > 0) {
            uint32_t chunk = (rem < sizeof(buf)) ? rem : sizeof(buf);
            for (uint32_t i = 0; i < chunk; i++) {
                seed = seed * 1103515245 + 12345;
                buf[i] = (seed >> 16) & 0xFF;
            }
            fwrite(buf, 1, chunk, f);
            fflush(f);
            rem -= chunk;
            // Throttle to match real USB write speed
            if (writeSpeedKBps > 0) {
                usleep(chunk * 1000 / writeSpeedKBps);
            }
        }
        fclose(f);
        return size;
    }

    void writeMetrics() {
        char path[384];

        // dsuMetric.eacmf (empty file)
        snprintf(path, sizeof(path), "%s/metrics/dsuMetric.eacmf", sdRoot);
        FILE* f = fopen(path, "wb"); if (f) fclose(f);

        // dsuMetric.{1-8}.eacmf (~20KB each)
        for (int i = 1; i <= 8; i++) {
            if (i == 7) continue;  // real data skips 7
            snprintf(path, sizeof(path), "%s/metrics/dsuMetric.%d.eacmf", sdRoot, i);

            if (metricsSource) {
                char srcPath[384];
                snprintf(srcPath, sizeof(srcPath), "%s/dsuMetric.%d.eacmf", metricsSource, i);
                copyFile(srcPath, path);
            } else {
                generateFile(path, 20 * 1024);  // ~20KB
            }
        }

        // dsuUsage.eacuf (~640 bytes)
        snprintf(path, sizeof(path), "%s/metrics/dsuUsage.eacuf", sdRoot);
        if (metricsSource) {
            char srcPath[384];
            snprintf(srcPath, sizeof(srcPath), "%s/dsuUsage.eacuf", metricsSource);
            copyFile(srcPath, path);
        } else {
            generateFile(path, 640);
        }
    }

    void writeReport(const char* filename, uint32_t bytes, const char* dateStr) {
        char path[384];
        snprintf(path, sizeof(path), "%s/downloadReport.txt", sdRoot);
        FILE* f = fopen(path, "w");
        if (!f) return;

        time_t now = time(nullptr);
        struct tm tm;
        localtime_r(&now, &tm);

        fprintf(f, "Gps date(yyyymmdd) and time(HHmmss) : %s %02d%02d%02d\n",
                dateStr, tm.tm_hour, tm.tm_min, tm.tm_sec);
        fprintf(f, "Downloading usage & metric to /mnt/memStick/metrics/ ...\n");
        fprintf(f, "Finished download usage & metric to /mnt/memStick/metrics/\n");
        fprintf(f, "Downloading FH to /mnt/memStick/flightHistory/%s ...\n", filename);
        fprintf(f, "  Estimated download file size is ~%u KB\n", bytes / 1024);
        fprintf(f, "Finished download FH to /mnt/memStick/flightHistory/%s\n", filename);
        fprintf(f, "Download Report:\n");
        fprintf(f, "  Size : %u bytes, ~%u KB\n", bytes, bytes / 1024);
        fprintf(f, "  Duration : %u sec or ~%u min\n", bytes / 1048576, bytes / 1048576 / 60);
        fclose(f);
    }
};

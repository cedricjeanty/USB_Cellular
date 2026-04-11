#pragma once
// AirBridge harvest logic — directory walking, file copying, skip detection
// Uses g_hal->filesys for all I/O. Extracted from doHarvest() in main.cpp.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_proto.h"
#include <cstdio>
#include <cstring>

struct HarvestResult {
    uint16_t count;
    float    usedMb;
    uint32_t maxFlight;
    char     dsuSerial[44];
};

// Walk a directory tree, copy new files to destDir, return harvest stats.
// srcDir:  root directory to scan (e.g. "/sdcard")
// destDir: destination directory (e.g. "/sdcard/harvested")
inline HarvestResult harvestFiles(const char* srcDir, const char* destDir) {
    HarvestResult result = {};
    if (!g_hal || !g_hal->filesys) return result;

    g_hal->filesys->mkdir(destDir);

    // Stack-based directory walk (max depth 4)
    struct DirFrame { void* dir; char prefix[80]; char dirpath[80]; };
    DirFrame stack[4];
    int depth = 0;
    strlcpy(stack[0].dirpath, srcDir, sizeof(stack[0].dirpath));
    stack[0].dir = g_hal->filesys->opendir(stack[0].dirpath);
    stack[0].prefix[0] = '\0';

    if (!stack[0].dir) return result;

    while (depth >= 0) {
        FsDirEntry ent;
        if (!g_hal->filesys->readdir(stack[depth].dir, &ent)) {
            g_hal->filesys->closedir(stack[depth].dir);
            depth--;
            continue;
        }

        if (isSkipped(ent.name)) continue;

        char fullpath[160];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", stack[depth].dirpath, ent.name);

        // readdir may not provide size/type — stat to get real values
        uint32_t statSize = 0;
        bool statIsDir = ent.is_dir;
        if (g_hal->filesys->stat(fullpath, &statSize, &statIsDir)) {
            ent.size = statSize;
            ent.is_dir = statIsDir;
        }

        if (ent.is_dir) {
            if (depth < 3) {
                depth++;
                snprintf(stack[depth].dirpath, sizeof(stack[depth].dirpath),
                         "%s/%s", stack[depth-1].dirpath, ent.name);
                stack[depth].dir = g_hal->filesys->opendir(stack[depth].dirpath);
                if (!stack[depth].dir) { depth--; continue; }
                if (stack[depth-1].prefix[0])
                    snprintf(stack[depth].prefix, sizeof(stack[depth].prefix),
                             "%s__%s", stack[depth-1].prefix, ent.name);
                else
                    strlcpy(stack[depth].prefix, ent.name, sizeof(stack[depth].prefix));
            }
            continue;
        }

        if (ent.size == 0) continue;

        // Build destination name
        char dstName[128];
        flattenPath(stack[depth].prefix, ent.name, dstName, sizeof(dstName));

        // Skip if already uploaded (.done marker exists)
        char donePath[192];
        snprintf(donePath, sizeof(donePath), "%s/.done__%s", destDir, dstName);
        if (g_hal->filesys->exists(donePath)) continue;

        // Skip if same-size copy already pending
        char dst[192];
        snprintf(dst, sizeof(dst), "%s/%s", destDir, dstName);
        uint32_t dstSize = 0;
        bool dstIsDir = false;
        if (g_hal->filesys->stat(dst, &dstSize, &dstIsDir)) {
            if (dstSize == ent.size) continue;
            g_hal->filesys->remove(dst);
        }

        // Copy file
        void* sf = g_hal->filesys->open(fullpath, "rb");
        void* df = g_hal->filesys->open(dst, "wb");
        bool copied = false;
        if (sf && df) {
            uint8_t cpbuf[4096];
            uint32_t rem = ent.size;
            copied = true;
            while (rem > 0) {
                size_t toRead = (rem < sizeof(cpbuf)) ? rem : sizeof(cpbuf);
                size_t n = g_hal->filesys->read(sf, cpbuf, toRead);
                if (n == 0 || g_hal->filesys->write(df, cpbuf, n) != n) { copied = false; break; }
                rem -= n;
            }
        }
        if (sf) g_hal->filesys->close(sf);
        if (df) g_hal->filesys->close(df);
        if (!copied) g_hal->filesys->remove(dst);

        if (copied) {
            float fileMb = (float)ent.size / 1e6f;
            result.usedMb += fileMb;
            result.count++;

            // Track DSU flight numbers from .eaofh files
            const char* ext = strrchr(ent.name, '.');
            if (ext && strcmp(ext, ".eaofh") == 0) {
                char serial[44];
                uint32_t fnum = 0;
                if (parseDsuFilename(ent.name, serial, sizeof(serial), &fnum) && fnum > result.maxFlight) {
                    result.maxFlight = fnum;
                    strlcpy(result.dsuSerial, serial, sizeof(result.dsuSerial));
                }
            }
        }
    }

    return result;
}

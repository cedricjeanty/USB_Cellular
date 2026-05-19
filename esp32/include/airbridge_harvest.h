#pragma once
// AirBridge harvest logic — directory walking, file moving to sequential folders
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
    char     folder[8];      // e.g. "0001"
};

// HAL filesys adapter for lastRecordFromLog. Wraps a HAL file handle so the
// content parser can do random-access reads via the same I/O abstraction.
struct HalLogReader {
    void* fh;            // HAL file handle (g_hal->filesys->open result)
};
inline uint32_t hal_read_at(void* ctx, uint64_t off, uint8_t* buf, uint32_t len) {
    HalLogReader* r = (HalLogReader*)ctx;
    if (!g_hal || !g_hal->filesys || !r->fh) return 0;
    if (!g_hal->filesys->seek(r->fh, (long)off, 0 /* SEEK_SET */)) return 0;
    return (uint32_t)g_hal->filesys->read(r->fh, buf, len);
}

// Scan srcDir (root + one subdirectory level) for any harvestable files.
// Returns true immediately on the first non-empty, non-skipped file found.
// Used at boot to detect files left behind by a power loss mid-transfer.
inline bool hasUnharvestedFiles(const char* srcDir) {
    if (!g_hal || !g_hal->filesys) return false;
    void* dir = g_hal->filesys->opendir(srcDir);
    if (!dir) return false;
    bool found = false;
    FsDirEntry ent;
    while (!found && g_hal->filesys->readdir(dir, &ent)) {
        if (ent.name[0] == '.') continue;
        if (strcmp(ent.name, "upload") == 0) continue;
        if (isSkipped(ent.name)) continue;
        char fp[160];
        snprintf(fp, sizeof(fp), "%s/%s", srcDir, ent.name);
        uint32_t sz = 0; bool isDir = false;
        g_hal->filesys->stat(fp, &sz, &isDir);
        if (isDir) {
            void* sub = g_hal->filesys->opendir(fp);
            if (!sub) continue;
            FsDirEntry se;
            while (g_hal->filesys->readdir(sub, &se)) {
                if (se.name[0] == '.') continue;
                char sfp[256];
                snprintf(sfp, sizeof(sfp), "%s/%s", fp, se.name);
                uint32_t ssz = 0; bool sd = false;
                g_hal->filesys->stat(sfp, &ssz, &sd);
                if (!sd && ssz > 0) { found = true; break; }
            }
            g_hal->filesys->closedir(sub);
        } else if (sz > 0) {
            found = true;
        }
    }
    g_hal->filesys->closedir(dir);
    return found;
}

// Walk srcDir, move files into destBase/NNNN/ (sequential subfolder).
// Files are copied then deleted from source (move across mount points).
// harvestNum: caller-provided sequential counter (read+increment NVS).
inline HarvestResult harvestFiles(const char* srcDir, const char* destBase,
                                   uint16_t harvestNum) {
    HarvestResult result = {};
    if (!g_hal || !g_hal->filesys) return result;

    snprintf(result.folder, sizeof(result.folder), "%04u", harvestNum);

    g_hal->filesys->mkdir(destBase);
    char destDir[192];
    snprintf(destDir, sizeof(destDir), "%s/%s", destBase, result.folder);
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

        // Build destination name (flatten subdirectory path)
        char dstName[128];
        flattenPath(stack[depth].prefix, ent.name, dstName, sizeof(dstName));

        char dst[256];
        snprintf(dst, sizeof(dst), "%s/%s", destDir, dstName);

        // Copy file to destination
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

        if (copied) {
            // Delete source (move = copy + delete)
            g_hal->filesys->remove(fullpath);

            float fileMb = (float)ent.size / 1e6f;
            result.usedMb += fileMb;
            result.count++;

            // Track DSU flight numbers from .eaofh files via content scan.
            // Backward-scan the *destination* copy (already-flushed) for the
            // last 0x0E/0x4C record; pull serial + flight from body offsets.
            const char* ext = strrchr(ent.name, '.');
            if (ext && strcmp(ext, ".eaofh") == 0) {
                void* fh = g_hal->filesys->open(dst, "rb");
                if (fh) {
                    HalLogReader rdr = { fh };
                    char serial[13] = {0};
                    uint32_t fnum = 0;
                    if (lastRecordFromLog(hal_read_at, &rdr, ent.size,
                                          &fnum, serial, sizeof(serial))) {
                        if (fnum > result.maxFlight) result.maxFlight = fnum;
                        // Serial is identical across all .eaofh from one DSU;
                        // record the first non-empty one we see.
                        if (serial[0] && !result.dsuSerial[0])
                            strlcpy(result.dsuSerial, serial, sizeof(result.dsuSerial));
                    }
                    g_hal->filesys->close(fh);
                }
            }
        } else {
            g_hal->filesys->remove(dst);
        }
    }

    return result;
}

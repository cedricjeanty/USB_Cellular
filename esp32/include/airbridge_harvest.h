#pragma once
// AirBridge harvest logic — directory walking, file moving to sequential folders
// Uses g_hal->filesys for all I/O. Extracted from doHarvest() in main.cpp.

#include "hal/hal.h"
#include "airbridge_utils.h"
#include "airbridge_proto.h"
#include <cstdio>
#include <cstring>

// Opt-in on-device gzip of .eaofh files into the upload queue (~3x fewer bytes on
// the cellular PUT). Gated by -DAIRBRIDGE_COMPRESS so contexts that don't link zlib
// (e.g. a firmware build before zlib is wired) are unaffected; the `compress` arg
// is simply ignored when the macro is off.
#include "hal/gzip_io.h"   // cz_hal_read/write + airbridge_compress.h (AIRBRIDGE_COMPRESS)

// Opt-in harvest tracing (-DHARVEST_TRACE): logs every directory opened and every
// entry the walk sees (name / is_dir / stat size / skip), so a serial capture shows
// EXACTLY where a host-written file disappears (readdir miss vs subdir-not-entered
// vs stat-size-0 — i.e. the MSC↔FATFS coherency question). Off by default so normal
// builds + the native tests don't pull in airbridge_log or spam.
#ifdef HARVEST_TRACE
#include "airbridge_log.h"
#define HTRACE(...) airbridge_log(__VA_ARGS__)
#else
#define HTRACE(...) ((void)0)
#endif

struct HarvestResult {
    uint16_t count;
    float    usedMb;
    uint32_t maxFlight;
    uint32_t minFlight;   // first flight seen across all .eaofh files
    char     dsuSerial[44];
    char     folder[8];      // e.g. "0001"
    uint64_t inBytes;     // total UNcompressed source bytes processed (gzip CPU time ∝ this;
                          // the emulator models the device's on-device gzip throughput off it)
};

// Flight number to write into the DSU cookie after a harvest. The cookie holds the LAST
// fully-downloaded flight N; the DSU resumes at N+1 (buildDsuCookie + upper_bound semantics).
// A NORMAL (15s-quiet) harvest sees the DSU's *finished* write, so every flight up to
// maxFlight is complete → cookie = maxFlight. A BOOT-RECOVERY harvest, though, may be
// reconciling a TRUNCATED interrupted transfer whose LAST flight (maxFlight) could be only
// PARTIALLY written — and lastRecordFromLog cannot tell a flight's mid record from its last —
// so back the cookie off by one to RE-REQUEST that flight (the DSU re-sends it whole next
// cycle). This never loses the tail of a partial flight; worst case it re-sends one
// already-complete flight, which is then harvested normally so the cookie advances (no loop).
inline uint32_t recoveryCookieFlight(uint32_t maxFlight, bool bootRecovery) {
    if (bootRecovery && maxFlight > 0) return maxFlight - 1;
    return maxFlight;
}

// Whether a completed harvest should advance the DSU cookie forward. Once real
// flights are safely on the SD, the cookie MUST move past them so the aircraft
// doesn't re-dump them on the next flight — this is driven by HARVEST, not by
// upload success (a slow/failed upload must never strand the cookie) and NOT by
// whether an operator staged a starting cookie this boot. An operator-staged S3
// cookie sets only the *starting* point (and still suppresses the boot
// manifest-sync); gating this forward advance on it was a bug: a staged cookie
// then made the unit re-dump its whole retained history on EVERY subsequent
// flight (found in the field on EA500.000243, 2026-07-09). A truncated
// boot-recovery harvest is excluded — recoveryCookieFlight handles its back-off.
inline bool harvestShouldAdvanceCookie(int filesHarvested, bool harvestIncomplete,
                                       uint32_t maxFlight, bool hasDsuSerial) {
    return filesHarvested > 0 && !harvestIncomplete && maxFlight > 0 && hasDsuSerial;
}

// HAL filesys adapter for the record parsers — moved to hal/log_reader.h so
// airbridge_s3.h can share it (delta split's firstRecordFromLog) without a
// harvest.h ↔ s3.h include cycle.
#include "hal/log_reader.h"

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
                                   uint16_t harvestNum, bool compress = false) {
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
    HTRACE("HARVEST: opendir %s -> %s", stack[0].dirpath, stack[0].dir ? "ok" : "FAIL");

    if (!stack[0].dir) return result;

    while (depth >= 0) {
        FsDirEntry ent;
        if (!g_hal->filesys->readdir(stack[depth].dir, &ent)) {
            HTRACE("HARVEST: readdir(%s) end (depth=%d)", stack[depth].dirpath, depth);
            g_hal->filesys->closedir(stack[depth].dir);
            depth--;
            continue;
        }

        if (isSkipped(ent.name)) { HTRACE("HARVEST: entry '%s' SKIPPED", ent.name); continue; }

        char fullpath[160];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", stack[depth].dirpath, ent.name);

        // readdir may not provide size/type — stat to get real values
        uint32_t statSize = 0;
        bool statIsDir = ent.is_dir;
        bool statOk = g_hal->filesys->stat(fullpath, &statSize, &statIsDir);
        if (statOk) {
            ent.size = statSize;
            ent.is_dir = statIsDir;
        }
        HTRACE("HARVEST: entry '%s' dir=%d statOk=%d size=%u (depth=%d)",
               ent.name, (int)ent.is_dir, (int)statOk, (unsigned)ent.size, depth);

        if (ent.is_dir) {
            if (depth < 3) {
                depth++;
                snprintf(stack[depth].dirpath, sizeof(stack[depth].dirpath),
                         "%s/%s", stack[depth-1].dirpath, ent.name);
                stack[depth].dir = g_hal->filesys->opendir(stack[depth].dirpath);
                HTRACE("HARVEST: descend %s -> %s", stack[depth].dirpath,
                       stack[depth].dir ? "ok" : "FAIL");
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

        char dst[300];
        snprintf(dst, sizeof(dst), "%s/%s", destDir, dstName);

        const char* ext = strrchr(ent.name, '.');
        bool isEaofh = ext && strcmp(ext, ".eaofh") == 0;
        // gzip .eaofh files in place (same .eaofh name, gzip content) when compress
        // is on: the upload streams ~3x fewer bytes, derives last_flight from the
        // filename + first_flight from the .meta written below (from the UNcompressed
        // source), and only ever scans content for split-delta — which never fires
        // for single-flight files (first==last). Non-.eaofh + non-compress copy verbatim.
        bool doGz = false;
#ifdef AIRBRIDGE_COMPRESS
        doGz = compress && isEaofh;
#else
        (void)compress;
#endif

        void* sf = g_hal->filesys->open(fullpath, "rb");
        void* df = g_hal->filesys->open(dst, "wb");
        bool copied = false;
        uint64_t storedBytes = 0;
#ifdef AIRBRIDGE_COMPRESS
        if (sf && df && doGz) {
            GzipResult gr = gzipStream(cz_hal_read, sf, cz_hal_write, df);
            HTRACE("HARVEST: gzip '%s' ok=%d in=%lu out=%lu", ent.name, (int)gr.ok,
                   (unsigned long)gr.inBytes, (unsigned long)gr.outBytes);
            if (gr.ok) {
                copied = true;
                storedBytes = gr.outBytes;
            } else {
                // Compression failed (e.g. the ~164KB ROM-tdefl deflate state won't
                // allocate on a board with PSRAM disabled) — fall back to a verbatim
                // copy so the flight is NEVER dropped. Reopen both: dst holds a partial
                // .gz to overwrite, src must rewind.
                HTRACE("HARVEST: gzip FAILED -> verbatim fallback '%s'", ent.name);
                g_hal->filesys->close(sf); g_hal->filesys->close(df);
                sf = g_hal->filesys->open(fullpath, "rb");
                df = g_hal->filesys->open(dst, "wb");
                doGz = false;  // fall through to verbatim below
            }
        }
#endif
        if (sf && df && !copied) {  // verbatim: non-.eaofh, non-compress build, or gzip fallback
            uint8_t cpbuf[4096];
            uint32_t rem = ent.size;
            copied = true;
            while (rem > 0) {
                size_t toRead = (rem < sizeof(cpbuf)) ? rem : sizeof(cpbuf);
                size_t n = g_hal->filesys->read(sf, cpbuf, toRead);
                if (n == 0 || g_hal->filesys->write(df, cpbuf, n) != n) { copied = false; break; }
                rem -= n;
            }
            storedBytes = ent.size;
        }
        if (sf) g_hal->filesys->close(sf);
        if (df) g_hal->filesys->close(df);

        if (copied) {
            // Track DSU flight numbers from .eaofh files by scanning the SOURCE
            // (uncompressed) before deletion — so the cookie/manifest + .meta are
            // correct even when dst holds gzip content. Backward-scan last_flight;
            // forward-scan first_flight; write a "{first}:{last}\n" .meta sidecar.
            if (isEaofh) {
                void* fh = g_hal->filesys->open(fullpath, "rb");
                if (fh) {
                    HalLogReader rdr = { fh };
                    char serial[13] = {0};
                    uint32_t lastFlight = 0, firstFlight = 0;
                    char serialFirst[13] = {0};

                    if (lastRecordFromLog(hal_read_at, &rdr, ent.size,
                                          &lastFlight, serial, sizeof(serial))) {
                        if (lastFlight > result.maxFlight) result.maxFlight = lastFlight;
                        if (serial[0] && !result.dsuSerial[0])
                            strlcpy(result.dsuSerial, serial, sizeof(result.dsuSerial));
                    }
                    if (firstRecordFromLog(hal_read_at, &rdr, ent.size,
                                           &firstFlight, serialFirst, sizeof(serialFirst))) {
                        if (result.minFlight == 0 || firstFlight < result.minFlight)
                            result.minFlight = firstFlight;
                    }
                    g_hal->filesys->close(fh);

                    if (firstFlight > 0 && lastFlight > 0) {
                        char metaDst[320];
                        snprintf(metaDst, sizeof(metaDst), "%s.meta", dst);
                        void* mf = g_hal->filesys->open(metaDst, "wb");
                        if (mf) {
                            char metaBuf[32];
                            int mlen = snprintf(metaBuf, sizeof(metaBuf),
                                                "%lu:%lu\n",
                                                (unsigned long)firstFlight,
                                                (unsigned long)lastFlight);
                            g_hal->filesys->write(mf, metaBuf, (size_t)mlen);
                            g_hal->filesys->close(mf);
                        }
                    }
                }
            }

            // Delete source (move = copy + delete)
            g_hal->filesys->remove(fullpath);
            result.usedMb += (float)storedBytes / 1e6f;
            result.inBytes += ent.size;   // uncompressed source bytes (gzip CPU time ∝ this)
            result.count++;
        } else {
            g_hal->filesys->remove(dst);
        }
    }

    return result;
}

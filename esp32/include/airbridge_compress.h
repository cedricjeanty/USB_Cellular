#pragma once
// Streaming gzip compression for flight logs before upload.
//
// Real .eaofh logs compress ~2.95x (22 MB fixture) to ~4.12x (1.49 GB fixture),
// so compressing on-device before the cellular PUT multiplies effective uplink
// throughput ~3x — the single biggest catch-up-time lever on a link-bound device
// (large files already hit the ~167 KB/s link cap, so the win is fewer bytes, not
// faster bytes). The device is link-bound, not CPU-bound, so it has the cycles.
//
// ONE backend everywhere: zlib, so the firmware, the SDL emulator, and the unit
// tests run the SAME compressor and produce matching output. The host links system
// zlib; the ESP32-S3 firmware links the espressif/zlib managed component (see
// src/idf_component.yml) — NOT the ROM miniz tdefl, whose ~164 KB compressor state
// can't allocate on this PCB variant (PSRAM disabled — see project notes).
//
// SMALL WINDOW (CZ_WINDOW_BITS=13 ⇒ 8 KB) keeps the deflate state ~64 KB so it fits
// the firmware's internal RAM (no PSRAM); zlib encodes the window size in the stream
// so the output is still plain `gunzip`-able. I/O is abstracted behind read/write
// function pointers (mirroring airbridge_proto.h's log_read_at_fn) so the same caller
// streams a HAL file (firmware/emulator) or a stdio FILE (tests), bounded memory
// regardless of file size.

#include <zlib.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Deflate config — shared by firmware + host so ratios + output + speed match.
// windowBits 13 (+16 for gzip framing) and memLevel 6 ⇒ deflate state ~64 KB (fits
// the ESP32-S3 internal heap with PSRAM disabled).
// CZ_LEVEL 1 (fastest): on the ESP32-S3 the deflate CPU — not the SD — is the
// bottleneck. MEASURED ON HARDWARE (8.39 MB flight, -DHARVEST_TRACE GZ:enter→GZ:ok):
//   level 6 → ~53 KB/s (SLOWER than the ~167 KB/s cellular link ⇒ compression a net
//             time LOSS: gzip runs serial before the PUT, so a sub-link gzip rate makes
//             the end-to-end slower even though it sends fewer bytes), and
//   level 1 → ~178 KB/s (47 s for 8,388,665 B) ⇒ ~3.4x faster, now ABOVE the link, so
//             compression is a time win again AND still ~2.7x fewer bytes (1.84x on the
//             higher-entropy real .eaofh). The 3.4x device speedup far exceeds the host's
//             1.9x because level 6's deep hash-chain search thrashes the ESP32's small
//             cache much harder than a desktop's. The emulator models ~178 KB/s (CZ_LEVEL).
// memLevel 6 + windowBits 13 ⇒ ~64 KB deflate state; with 4 KB I/O buffers the harvest-time
// allocation (~72 KB) fits the free heap (16 KB buffers needed ~96 KB and FAILED to alloc
// mid-harvest — heap was only ~53 KB free during gzip — silently falling back to verbatim).
#define CZ_WINDOW_BITS 13
#define CZ_MEM_LEVEL   6
#define CZ_LEVEL       1
// Modeled on-device level-1 gzip throughput (bytes/s), for the emulator to match hardware.
#define CZ_DEVICE_GZIP_BPS 178000

// Read up to `n` bytes into buf; return bytes read (0 = EOF). Must fill until EOF
// (short read ⇒ EOF), as stdio fread and FatFs f_read both do.
typedef size_t (*cz_read_fn)(void* ctx, void* buf, size_t n);
// Write exactly `n` bytes; return bytes written (< n ⇒ error, aborts the stream).
typedef size_t (*cz_write_fn)(void* ctx, const void* buf, size_t n);

struct GzipResult {
    bool     ok;
    uint64_t inBytes;
    uint64_t outBytes;
};

// Opt-in gzip-path tracing (-DHARVEST_TRACE) to surface a firmware compress failure
// (e.g. deflateInit2 Z_MEM_ERROR if the state won't allocate) on the serial log.
#ifdef HARVEST_TRACE
#include "airbridge_log.h"
#define GZTRACE(...) airbridge_log(__VA_ARGS__)
#else
#define GZTRACE(...) ((void)0)
#endif

// Stream-gzip from a read source to a write sink. Bounded memory: zlib's deflate
// state (~64 KB heap at the config above) + two 4 KB heap buffers (kept small so the
// total harvest-time allocation fits free heap). Chunked, so safe for arbitrarily
// large files. level 1..9 (CZ_LEVEL=1 default — fastest; on the ESP32 ~3.4x quicker
// than level 6 so on-device gzip beats the cellular link; 1 is markedly faster
// for ~5% worse ratio).
inline GzipResult gzipStream(cz_read_fn rd, void* rctx, cz_write_fn wr, void* wctx,
                             int level = CZ_LEVEL) {
    GzipResult r = {false, 0, 0};
    if (!rd || !wr) return r;
    GZTRACE("GZ: enter");

    // Buffers on the HEAP, not the stack (16 KB stack buffers overflowed the harvest
    // task stack and crash-looped the device). 4 KB chunks: larger buffers + the ~64 KB
    // deflate state exhausted the free heap at harvest time (gzip failed → verbatim
    // fallback → uncompressed upload), and the deflate CPU — not SD-op overhead — is the
    // throughput bottleneck, so bigger buffers wouldn't help anyway.
    const size_t BUF = 4096;
    unsigned char* inbuf  = (unsigned char*)malloc(BUF);
    unsigned char* outbuf = (unsigned char*)malloc(BUF);
    if (!inbuf || !outbuf) { GZTRACE("GZ: buf alloc fail"); free(inbuf); free(outbuf); return r; }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    int initRc = deflateInit2(&zs, level, Z_DEFLATED, CZ_WINDOW_BITS + 16,
                              CZ_MEM_LEVEL, Z_DEFAULT_STRATEGY);
    GZTRACE("GZ: deflateInit2 rc=%d", initRc);
    if (initRc != Z_OK) { free(inbuf); free(outbuf); return r; }

    int flush = Z_NO_FLUSH;
    do {
        size_t got = rd(rctx, inbuf, BUF);
        r.inBytes += got;
        if (got == 0) flush = Z_FINISH;   // EOF (or empty input) — drain + finalize
        zs.next_in = inbuf;
        zs.avail_in = (uInt)got;

        do {
            zs.next_out = outbuf;
            zs.avail_out = (uInt)BUF;
            int ret = deflate(&zs, flush);
            if (ret == Z_STREAM_ERROR) { GZTRACE("GZ: deflate STREAM_ERROR"); deflateEnd(&zs); free(inbuf); free(outbuf); return r; }
            size_t have = BUF - zs.avail_out;
            if (have) {
                if (wr(wctx, outbuf, have) != have) { GZTRACE("GZ: write FAILED"); deflateEnd(&zs); free(inbuf); free(outbuf); return r; }
                r.outBytes += have;
            }
        } while (zs.avail_out == 0);
    } while (flush != Z_FINISH);

    deflateEnd(&zs);
    free(inbuf); free(outbuf);
    r.ok = true;
    GZTRACE("GZ: ok in=%llu out=%llu", (unsigned long long)r.inBytes, (unsigned long long)r.outBytes);
    return r;
}

// stdio adapters (unit tests; also handy for host tooling).
inline size_t cz_stdio_read(void* ctx, void* buf, size_t n) {
    return fread(buf, 1, n, (FILE*)ctx);
}
inline size_t cz_stdio_write(void* ctx, const void* buf, size_t n) {
    return fwrite(buf, 1, n, (FILE*)ctx);
}

// Convenience: gzip one stdio path to another. Returns the result (ok=false on any
// open/deflate error). The HAL-backed equivalent used at harvest lives next to the
// harvest code so this header stays HAL-free.
inline GzipResult gzipFileStdio(const char* srcPath, const char* dstPath, int level = CZ_LEVEL) {
    GzipResult r = {false, 0, 0};
    FILE* sf = fopen(srcPath, "rb");
    if (!sf) return r;
    FILE* df = fopen(dstPath, "wb");
    if (!df) { fclose(sf); return r; }
    r = gzipStream(cz_stdio_read, sf, cz_stdio_write, df, level);
    fclose(sf);
    if (fclose(df) != 0) r.ok = false;
    return r;
}

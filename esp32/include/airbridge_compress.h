#pragma once
// Streaming gzip compression for flight logs before upload.
//
// Real .eaofh logs compress ~2.95x (22 MB fixture) to ~4.12x (1.49 GB fixture),
// so compressing on-device before the cellular PUT multiplies effective uplink
// throughput ~3x — the single biggest catch-up-time lever on a link-bound device
// (measured: large files already hit the ~167 KB/s link cap, so the win is fewer
// bytes, not faster bytes). The device is link-bound, not CPU-bound, so it has the
// cycles for deflate.
//
// Standard gzip framing so the S3 consumer can plain `gunzip`. Two backends behind
// ONE signature: on the host (native tests + SDL emulator) system zlib; on the
// ESP32-S3 the ROM's miniz `tdefl` (exported from ROM — zero flash, zero fetch) with
// the deflate state in PSRAM. Both emit standard deflate, so output is interchangeable.
// I/O is abstracted behind read/write function pointers (mirroring airbridge_proto.h's
// log_read_at_fn) so the same caller streams a HAL file (firmware/emulator) or a stdio
// FILE (tests), with bounded memory regardless of file size.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>

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

#if defined(ESP_PLATFORM)
// ── Firmware: ROM miniz tdefl + manual gzip framing ──────────────────────────
// tdefl_* and mz_crc32 live in the ESP32-S3 ROM (esp32s3.rom.ld) — no flash cost,
// no component fetch. The tdefl_compressor (~110 KB w/ ROM's TDEFL_LESS_MEMORY) is
// allocated from PSRAM; input is a 4 KB stack buffer (harvest task stack is 16 KB)
// and deflate output is pushed through tdefl's callback straight to the sink.
#include "miniz.h"
#include "esp_heap_caps.h"

// Opt-in gzip-path tracing (-DHARVEST_TRACE) to pinpoint a firmware compress failure.
#ifdef HARVEST_TRACE
#include "airbridge_log.h"
#define GZTRACE(...) airbridge_log(__VA_ARGS__)
#else
#define GZTRACE(...) ((void)0)
#endif

struct CzTdeflCtx { cz_write_fn wr; void* wctx; bool ok; uint64_t out; };
inline mz_bool cz_tdefl_put(const void* buf, int len, void* user) {
    CzTdeflCtx* c = (CzTdeflCtx*)user;
    if (!c->ok) return MZ_FALSE;
    if (c->wr(c->wctx, buf, (size_t)len) != (size_t)len) { c->ok = false; return MZ_FALSE; }
    c->out += (size_t)len;
    return MZ_TRUE;
}

inline GzipResult gzipStream(cz_read_fn rd, void* rctx, cz_write_fn wr, void* wctx,
                             int level = 6) {
    GzipResult r = {false, 0, 0};
    if (!rd || !wr) return r;

    bool fromPsram = true;
    tdefl_compressor* d = (tdefl_compressor*)heap_caps_malloc(sizeof(tdefl_compressor), MALLOC_CAP_SPIRAM);
    if (!d) { fromPsram = false; d = (tdefl_compressor*)malloc(sizeof(tdefl_compressor)); }  // fall back to internal RAM
    GZTRACE("GZ: alloc tdefl size=%u psram=%d ptr=%p", (unsigned)sizeof(tdefl_compressor),
            (int)fromPsram, (void*)d);
    if (!d) return r;

    CzTdeflCtx ctx = { wr, wctx, true, 0 };
    static const unsigned char gzhdr[10] = {0x1f,0x8b,0x08,0x00,0,0,0,0,0,0xff};
    if (wr(wctx, gzhdr, 10) != 10) { GZTRACE("GZ: header write FAILED"); free(d); return r; }
    ctx.out += 10;

    int probes = (level <= 1) ? 1 : (level >= 9 ? 4095 : TDEFL_DEFAULT_MAX_PROBES);
    tdefl_status initSt = tdefl_init(d, cz_tdefl_put, &ctx, probes);
    GZTRACE("GZ: tdefl_init st=%d", (int)initSt);
    if (initSt != TDEFL_STATUS_OKAY) { free(d); return r; }

    mz_ulong crc = mz_crc32(0, NULL, 0);
    uint64_t isize = 0;
    uint8_t inbuf[4096];
    bool done = false;
    while (!done && ctx.ok) {
        size_t got = rd(rctx, inbuf, sizeof(inbuf));
        isize += got;
        crc = mz_crc32(crc, inbuf, got);
        tdefl_flush flush = (got < sizeof(inbuf)) ? TDEFL_FINISH : TDEFL_NO_FLUSH;
        tdefl_status st = tdefl_compress_buffer(d, inbuf, got, flush);
        if (flush == TDEFL_FINISH) done = (st == TDEFL_STATUS_DONE);
        if (st != TDEFL_STATUS_OKAY && st != TDEFL_STATUS_DONE) {
            GZTRACE("GZ: compress_buffer st=%d at in=%llu out=%llu writeOk=%d", (int)st,
                    (unsigned long long)isize, (unsigned long long)ctx.out, (int)ctx.ok);
            ctx.ok = false;
        }
    }
    free(d);
    GZTRACE("GZ: loop end ok=%d done=%d in=%llu out=%llu", (int)ctx.ok, (int)done,
            (unsigned long long)isize, (unsigned long long)ctx.out);
    if (!ctx.ok || !done) return r;

    unsigned char tr[8];
    uint32_t c32 = (uint32_t)crc, isz = (uint32_t)isize;  // gzip trailer: CRC32 + ISIZE, both LE
    tr[0]=c32&0xff; tr[1]=(c32>>8)&0xff; tr[2]=(c32>>16)&0xff; tr[3]=(c32>>24)&0xff;
    tr[4]=isz&0xff; tr[5]=(isz>>8)&0xff; tr[6]=(isz>>16)&0xff; tr[7]=(isz>>24)&0xff;
    if (wr(wctx, tr, 8) != 8) return r;
    ctx.out += 8;

    r.inBytes = isize; r.outBytes = ctx.out; r.ok = true;
    return r;
}

#else
// ── Host (native tests + emulator): system zlib ──────────────────────────────
#include <zlib.h>

// Stream-gzip from a read source to a write sink. Bounded memory: zlib's deflate
// state (heap) + two 16 KB stack buffers. Chunked, so safe for arbitrarily large
// files. level 1..9 (6 = default; 1 is markedly faster for ~5% worse ratio).
inline GzipResult gzipStream(cz_read_fn rd, void* rctx, cz_write_fn wr, void* wctx,
                             int level = 6) {
    GzipResult r = {false, 0, 0};
    if (!rd || !wr) return r;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return r;

    const size_t BUF = 16384;
    unsigned char inbuf[BUF];
    unsigned char outbuf[BUF];
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
            if (ret == Z_STREAM_ERROR) { deflateEnd(&zs); return r; }
            size_t have = BUF - zs.avail_out;
            if (have) {
                if (wr(wctx, outbuf, have) != have) { deflateEnd(&zs); return r; }
                r.outBytes += have;
            }
        } while (zs.avail_out == 0);
    } while (flush != Z_FINISH);

    deflateEnd(&zs);
    r.ok = true;
    return r;
}
#endif

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
inline GzipResult gzipFileStdio(const char* srcPath, const char* dstPath, int level = 6) {
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

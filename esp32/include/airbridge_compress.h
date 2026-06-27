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
// zlib-based gzip framing (windowBits 15+16) so the S3 consumer can plain `gunzip`.
// On the host (native tests + SDL emulator) this links system zlib; on the ESP32-S3
// the firmware links zlib (ESP-IDF component) or wraps the ROM miniz `tdefl` — both
// produce standard deflate. I/O is abstracted behind read/write function pointers
// (mirroring airbridge_proto.h's log_read_at_fn) so the same code streams a HAL
// file in the firmware/emulator and a stdio FILE in unit tests, with bounded memory
// regardless of file size.

#include <zlib.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdio>

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

// Stream-gzip from a read source to a write sink. Bounded memory: zlib's deflate
// state (heap, ~256 KB at level 6 — on the ESP32 allocate from PSRAM) + two 16 KB
// stack buffers. Chunked, so safe for arbitrarily large files. level 1..9
// (6 = default; 1 is markedly faster for ~5% worse ratio — a sensible MCU default
// if deflate time ever dominates, which on a link-bound device it does not).
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
        // avail_in must be fully consumed before the next read.
    } while (flush != Z_FINISH);

    deflateEnd(&zs);
    r.ok = true;
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

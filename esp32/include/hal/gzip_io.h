#pragma once
// Shared HAL filesys ↔ gzipStream (airbridge_compress.h) I/O adapters. Kept in
// one place so both airbridge_harvest.h (harvest-time, historical) and
// airbridge_s3.h (upload-time, dedup-then-compress) use a single definition —
// otherwise a TU including both (main.cpp) redefines them. Gated by
// AIRBRIDGE_COMPRESS since it pulls in zlib via airbridge_compress.h.
#ifdef AIRBRIDGE_COMPRESS
#include "hal/hal.h"
#include "airbridge_compress.h"
inline size_t cz_hal_read(void* ctx, void* buf, size_t n) {
    return g_hal->filesys->read(ctx, buf, n);
}
inline size_t cz_hal_write(void* ctx, const void* buf, size_t n) {
    return g_hal->filesys->write(ctx, (void*)buf, n);
}
#endif

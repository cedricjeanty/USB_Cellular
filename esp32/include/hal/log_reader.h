#pragma once
// HAL filesys adapter for the .eaofh record parsers (firstRecordFromLog /
// lastRecordFromLog in airbridge_proto.h). Wraps a HAL file handle so the
// content parser does random-access reads through the same I/O abstraction on
// firmware, emulator, and native tests. Kept in its own tiny header so both
// airbridge_harvest.h and airbridge_s3.h can use it without pulling each other
// (or compress.h/zlib) in.
#include "hal/hal.h"
#include <cstdint>

struct HalLogReader {
    void* fh;            // HAL file handle (g_hal->filesys->open result)
};
inline uint32_t hal_read_at(void* ctx, uint64_t off, uint8_t* buf, uint32_t len) {
    HalLogReader* r = (HalLogReader*)ctx;
    if (!g_hal || !g_hal->filesys || !r->fh) return 0;
    if (!g_hal->filesys->seek(r->fh, (long)off, 0 /* SEEK_SET */)) return 0;
    return (uint32_t)g_hal->filesys->read(r->fh, buf, len);
}

#pragma once
// AirBridge shared SD service — hardware-independent SD/FatFs orchestration that
// compiles into BOTH the firmware and the emulator/tests, so they run the SAME
// code instead of hand-synced copies.
//
// Phase 1: the dual-partition FORMAT sequence (f_fdisk + MBR fixup + f_mkfs of
// P1 and P2 with FM_SFD). This was triplicated — the firmware's FmtTask, the
// block test's run_full_format, and FatFsFilesys — and is the riskiest SD code
// (a bug bricks the card). Extracting it here means the unit tests exercise the
// exact bytes the device writes.
//
// The platform differences (which block device backs FatFs, how diskio is
// registered, how sector 0 is read/written) are injected via SdDiskOps. The
// firmware backs it with sdmmc + ff_diskio_register; the emulator/tests back it
// with FakeSd. The SEQUENCE is shared.

#include <cstdint>
#include <cstddef>
#include "ff.h"   // resolves to ESP-IDF FatFs (firmware) or the vendored/lib FatFs (native)

// FatFs logical drive numbers used across the codebase (firmware + tests):
//   0 = whole card (raw) — used by f_fdisk
//   1 = P2 (internal) offset window
//   2 = P1 (DSU-facing) offset window
#ifndef SD_PDRV_RAW
#define SD_PDRV_RAW 0
#define SD_PDRV_P2  1
#define SD_PDRV_P1  2
#endif

// Platform hooks the shared format sequence drives. ctx is passed back to each
// callback (the firmware ignores it and touches globals; tests pass their FakeSd).
struct SdDiskOps {
    // Register drive 0 as the whole raw card (for f_fdisk).
    void (*registerRaw)(void* ctx);
    // Register `pdrv` (SD_PDRV_P1 or SD_PDRV_P2) as an offset window at LBA
    // `start` spanning `count` sectors (for f_mkfs of that partition).
    void (*registerOffset)(int pdrv, uint32_t start, uint32_t count, void* ctx);
    // Unregister a drive's diskio.
    void (*unregister)(int pdrv, void* ctx);
    // Read / write absolute sector 0 (the MBR). Return true on success.
    bool (*readMbr)(uint8_t* buf512, void* ctx);
    bool (*writeMbr)(const uint8_t* buf512, void* ctx);
    void* ctx;
};

struct SdFormatResult {
    bool     ok;                 // both partitions formatted
    int      fdiskFr;            // FRESULT of f_fdisk (FR_OK==0)
    int      mkfsP1Fr;           // FRESULT of f_mkfs P1
    int      mkfsP2Fr;           // FRESULT of f_mkfs P2 (valid only if reached)
    uint32_t p1Start, p1Size;    // LBA layout f_fdisk actually produced
    uint32_t p2Start, p2Size;
};

static inline uint32_t sdLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Partition the whole card into P1 (`p1Sectors`, capped/DSU-facing) + P2 (rest)
// and format both as FAT32 with FM_SFD. `auSize` is the cluster size in bytes
// (firmware uses 16K; tests 4K). `work`/`workSz` is the f_mkfs scratch buffer.
//
// Mirrors FmtTask exactly: f_fdisk writes a placeholder type 0x07, which we
// rewrite to 0x0C (FAT32 LBA) after reading back the CHS-aligned LBA starts;
// FM_SFD makes f_mkfs write each partition's VBR at the partition's own LBA
// start (rather than auto-detecting and clobbering the MBR with a single-
// partition table). On success the caller mounts P2/P1 and records the layout.
inline SdFormatResult sdFormatDual(const SdDiskOps& ops, uint32_t cardSectors,
                                   uint32_t p1Sectors, uint32_t auSize,
                                   void* work, size_t workSz) {
    SdFormatResult res = {};
    uint32_t p2Sectors = (cardSectors > p1Sectors + 2048) ? cardSectors - p1Sectors : 0;
    if (p2Sectors == 0 || !work) { res.fdiskFr = FR_INVALID_PARAMETER; return res; }

    // NB: the raw drive (0) is left registered on every exit path — the original
    // FmtTask did the same (normal operation keeps drive 0 registered), and the
    // tests reset all diskio via tearDown/reset_fatfs_state. The caller owns
    // drive 0's lifecycle.
    ops.unregister(SD_PDRV_RAW, ops.ctx);   // clear any stale registration
    ops.registerRaw(ops.ctx);

    // Step 1: partition table.
    LBA_t plist[] = { (LBA_t)p1Sectors, (LBA_t)p2Sectors, 0, 0 };
    res.fdiskFr = f_fdisk(SD_PDRV_RAW, plist, work);
    if (res.fdiskFr != FR_OK) return res;

    // Step 2: read back the actual (CHS-aligned) LBA starts + fix partition types.
    uint8_t mbr[512];
    if (ops.readMbr(mbr, ops.ctx)) {
        res.p1Start = sdLe32(mbr + 0x1BE + 8);
        res.p1Size  = sdLe32(mbr + 0x1BE + 12);
        res.p2Start = sdLe32(mbr + 0x1CE + 8);
        res.p2Size  = sdLe32(mbr + 0x1CE + 12);
        mbr[0x1BE + 4] = 0x0C;
        if (res.p2Size > 0) mbr[0x1CE + 4] = 0x0C;
        ops.writeMbr(mbr, ops.ctx);
    }

    MKFS_PARM opt = { (BYTE)(FM_FAT32 | FM_SFD), 2, 0, 0, auSize };

    // Step 3: format P1 via its offset window.
    ops.registerOffset(SD_PDRV_P1, res.p1Start, p1Sectors, ops.ctx);
    res.mkfsP1Fr = f_mkfs("2:", &opt, work, (UINT)workSz);
    ops.unregister(SD_PDRV_P1, ops.ctx);
    if (res.mkfsP1Fr != FR_OK) return res;

    // Step 4: format P2 via its offset window. (P2's diskio is left registered,
    // as FmtTask does, so the caller can mount "1:" right after.)
    if (res.p2Start > 0 && res.p2Size > 0) {
        ops.registerOffset(SD_PDRV_P2, res.p2Start, res.p2Size, ops.ctx);
        res.mkfsP2Fr = f_mkfs("1:", &opt, work, (UINT)workSz);
        if (res.mkfsP2Fr == FR_OK) res.ok = true;
    }
    return res;
}

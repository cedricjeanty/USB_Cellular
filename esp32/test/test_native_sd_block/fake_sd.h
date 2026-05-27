// FakeSd — a sparse in-memory SD card for native FatFs testing.
//
// Backed by std::map<sector, 512-byte array> so even a "16GB disk" only
// allocates memory for sectors that are actually written.  Supports up to
// FF_VOLUMES drives registered via the native diskio dispatch.
//
// Typical use:
//   FakeSd disk(31116288);           // 16 GB
//   disk.register_raw(0);            // drive 0: full disk
//   FakeSd::OffsetDisk p1{&disk, 63,            MSC_MAX};  // P1 view
//   FakeSd::OffsetDisk p2{&disk, p2_start, p2_size};       // P2 view
//   p1.register_as(2);  p2.register_as(1);

#pragma once
#include <cstdint>
#include <array>
#include <map>
#include "ff.h"
#include "diskio.h"

extern "C" {
void diskio_register_native(BYTE pdrv,
    DSTATUS (*init)(BYTE),
    DSTATUS (*status)(BYTE),
    DRESULT (*read)(BYTE, BYTE*, LBA_t, UINT),
    DRESULT (*write)(BYTE, const BYTE*, LBA_t, UINT),
    DRESULT (*ioctl)(BYTE, BYTE, void*));
void diskio_unregister_native(BYTE pdrv);
}

// ── FakeSd ────────────────────────────────────────────────────────────────

class FakeSd {
public:
    static constexpr uint32_t SECTOR_SIZE = 512;

    explicit FakeSd(uint32_t num_sectors) : num_sectors_(num_sectors) {}

    uint32_t num_sectors() const { return num_sectors_; }

    // Read count sectors starting at sector.  Unwritten sectors read as zeros.
    DRESULT read(uint8_t* buf, uint32_t sector, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            auto it = sectors_.find(sector + i);
            if (it != sectors_.end())
                memcpy(buf + i * SECTOR_SIZE, it->second.data(), SECTOR_SIZE);
            else
                memset(buf + i * SECTOR_SIZE, 0, SECTOR_SIZE);
        }
        return RES_OK;
    }

    DRESULT write(const uint8_t* buf, uint32_t sector, uint32_t count) {
        for (uint32_t i = 0; i < count; i++) {
            auto& s = sectors_[sector + i];
            memcpy(s.data(), buf + i * SECTOR_SIZE, SECTOR_SIZE);
        }
        return RES_OK;
    }

    // Read a single raw sector (no diskio overhead — for MBR inspection in tests)
    bool read_sector(uint32_t sector, uint8_t* out) {
        auto it = sectors_.find(sector);
        if (it != sectors_.end()) { memcpy(out, it->second.data(), SECTOR_SIZE); return true; }
        memset(out, 0, SECTOR_SIZE); return false;
    }

    size_t allocated_sector_count() const { return sectors_.size(); }

    // ── Register as raw disk on pdrv ──────────────────────────────────────
    void register_raw(BYTE pdrv) {
        g_instances[pdrv] = this;
        g_offsets[pdrv] = 0;
        g_sizes[pdrv] = num_sectors_;
        diskio_register_native(pdrv, s_init, s_status, s_read, s_write, s_ioctl);
    }

    // ── Offset view (for P1/P2 partition formatting) ──────────────────────
    // Registers a "window" into this disk starting at lba_offset with size
    // lba_count as drive pdrv.  f_mkfs on this drive sees only the partition.
    void register_offset(BYTE pdrv, uint32_t lba_offset, uint32_t lba_count) {
        g_instances[pdrv] = this;
        g_offsets[pdrv] = lba_offset;
        g_sizes[pdrv] = lba_count;
        diskio_register_native(pdrv, s_init, s_status, s_read_off, s_write_off, s_ioctl_off);
    }

    static void unregister(BYTE pdrv) {
        g_instances[pdrv] = nullptr;
        diskio_unregister_native(pdrv);
    }

private:
    uint32_t num_sectors_;
    std::map<uint32_t, std::array<uint8_t, SECTOR_SIZE>> sectors_;

    // Per-drive state (up to FF_VOLUMES drives)
    static FakeSd*  g_instances[FF_VOLUMES];
    static uint32_t g_offsets[FF_VOLUMES];
    static uint32_t g_sizes[FF_VOLUMES];

    // ── Raw disk diskio callbacks ─────────────────────────────────────────
    static DSTATUS s_init(BYTE pdrv)   { return g_instances[pdrv] ? 0 : STA_NOINIT; }
    static DSTATUS s_status(BYTE pdrv) { return g_instances[pdrv] ? 0 : STA_NOINIT; }

    static DRESULT s_read(BYTE pdrv, BYTE* buf, LBA_t sec, UINT cnt) {
        auto* d = g_instances[pdrv];
        return d ? d->read(buf, (uint32_t)sec, cnt) : RES_PARERR;
    }
    static DRESULT s_write(BYTE pdrv, const BYTE* buf, LBA_t sec, UINT cnt) {
        auto* d = g_instances[pdrv];
        return d ? d->write(buf, (uint32_t)sec, cnt) : RES_PARERR;
    }
    static DRESULT s_ioctl(BYTE pdrv, BYTE cmd, void* buf) {
        auto* d = g_instances[pdrv];
        if (!d) return RES_PARERR;
        switch (cmd) {
            case CTRL_SYNC:        return RES_OK;
            case GET_SECTOR_COUNT: *(DWORD*)buf = d->num_sectors_; return RES_OK;
            case GET_SECTOR_SIZE:  *(WORD*)buf  = SECTOR_SIZE;     return RES_OK;
            case GET_BLOCK_SIZE:   *(DWORD*)buf = 1;               return RES_OK;
            default: return RES_PARERR;
        }
    }

    // ── Offset (partition window) diskio callbacks ────────────────────────
    static DRESULT s_read_off(BYTE pdrv, BYTE* buf, LBA_t sec, UINT cnt) {
        auto* d = g_instances[pdrv];
        return d ? d->read(buf, (uint32_t)sec + g_offsets[pdrv], cnt) : RES_PARERR;
    }
    static DRESULT s_write_off(BYTE pdrv, const BYTE* buf, LBA_t sec, UINT cnt) {
        auto* d = g_instances[pdrv];
        return d ? d->write(buf, (uint32_t)sec + g_offsets[pdrv], cnt) : RES_PARERR;
    }
    static DRESULT s_ioctl_off(BYTE pdrv, BYTE cmd, void* buf) {
        auto* d = g_instances[pdrv];
        if (!d) return RES_PARERR;
        switch (cmd) {
            case CTRL_SYNC:        return RES_OK;
            case GET_SECTOR_COUNT: *(DWORD*)buf = g_sizes[pdrv]; return RES_OK;
            case GET_SECTOR_SIZE:  *(WORD*)buf  = SECTOR_SIZE;   return RES_OK;
            case GET_BLOCK_SIZE:   *(DWORD*)buf = 1;             return RES_OK;
            default: return RES_PARERR;
        }
    }
};

// Static member definitions (in the header for single-TU test builds)
inline FakeSd*  FakeSd::g_instances[FF_VOLUMES] = {};
inline uint32_t FakeSd::g_offsets[FF_VOLUMES]   = {};
inline uint32_t FakeSd::g_sizes[FF_VOLUMES]     = {};

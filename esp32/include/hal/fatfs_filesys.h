#pragma once
// FatFsFilesys — an IFilesys backed by REAL FatFs running on an in-memory
// FakeSd block device, in the SAME dual-partition layout the device uses. This
// is the emulator's "SD through FakeSd": file/dir operations execute the real
// FatFs code, and the card is partitioned + formatted by the shared
// airbridge_sd.h::sdFormatDual — the exact sequence the firmware's FmtTask runs.
//
// Layout (mirrors the hardware): one FakeSd card split into
//   P1 (DSU-facing)  -> FatFs drive "2:", HAL prefix p1Prefix (./emu_sdcard)
//   P2 (internal)    -> FatFs drive "1:", HAL prefix p2Prefix (./emu_sdcard_internal)
// HAL paths are mapped to the right drive by prefix.
//
// Corruption model: corruptFat() zeroes P2's VBR+FAT head (real sectors) and
// drops the mount, so subsequent ops fail and a remount fails — exactly a card
// whose filesystem is unreadable. format() re-runs sdFormatDual, which rewrites
// the partition table + VBRs and recovers a mountable, writable card (data lost —
// the DSU re-sends via the cookie), mirroring the device's reformat path.
//
// Single-threaded use assumed (the emulator serializes SD access under the HAL
// lock), matching FatFs's static-LFN-buffer config.

#include "hal/filesys.h"
#include "hal/fake_sd.h"
#include "ff.h"
#include "airbridge_sd.h"   // shared dual-partition format sequence

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <dirent.h>
#include <sys/stat.h>

class FatFsFilesys : public IFilesys {
public:
    // cardSectors: whole-card size; p1Sectors: size of P1 (P2 takes the rest).
    FatFsFilesys(uint32_t cardSectors, uint32_t p1Sectors,
                 const char* p1Prefix, const char* p2Prefix)
        : cardSectors_(cardSectors), p1Sectors_(p1Sectors), p1_(p1Prefix), p2_(p2Prefix) {
        disk_ = new FakeSd(cardSectors);
    }
    ~FatFsFilesys() { unmountAll(); FakeSd::unregister(SD_PDRV_RAW); delete disk_; }

    bool mounted() const { return mounted_; }

    // (Re)partition + format the card via the shared sequence, then mount both
    // partitions. Returns true on success.
    bool format() {
        unmountAll();
        FakeSd::unregister(SD_PDRV_RAW);
        FakeSd::unregister(SD_PDRV_P1);
        FakeSd::unregister(SD_PDRV_P2);
        static BYTE work[4096];
        SdDiskOps ops = makeOps();
        SdFormatResult r = sdFormatDual(ops, cardSectors_, p1Sectors_, 4096, work, sizeof(work));
        if (!r.ok) return false;
        p1Start_ = r.p1Start; p2Start_ = r.p2Start; p2Size_ = r.p2Size;
        return mountBoth();
    }

    // Attempt to (re)mount both partitions — used by the health/recovery loop.
    bool remount() {
        if (mounted_) return true;
        return mountBoth();
    }

    // Inject real FAT corruption: zero P2's VBR + FAT head and drop the mount.
    // Subsequent ops fail and remount() fails until format().
    void corruptFat() {
        uint8_t zero[512] = {0};
        for (uint32_t s = 0; s < 64; s++) disk_->write(zero, p2Start_ + s, 1);
        unmountAll();
    }

    // Light health probe (mirrors sd_health_check's f_stat on P2): true if the
    // P2 filesystem responds. When unmounted, tries a remount first.
    bool probeOk() {
        if (!mounted_ && !remount()) return false;
        FILINFO fno;
        FRESULT fr = f_stat("1:/logs", &fno);
        return (fr == FR_OK || fr == FR_NO_FILE);
    }

    // Copy a host directory tree into the card under the given HAL prefix, so the
    // emulator's "drop files into ./emu_sdcard" workflow still seeds the card.
    int importHostTree(const char* hostDir, const char* halPrefix) {
        if (!mounted_) return 0;
        return importRec(hostDir, halPrefix);
    }

    // ── IFilesys ────────────────────────────────────────────────────────────
    void* open(const char* path, const char* mode) override {
        if (!mounted_) return nullptr;
        BYTE m = parseMode(mode);
        if (!m) return nullptr;
        FIL* fp = new FIL();
        if (f_open(fp, mapPath(path).c_str(), m) != FR_OK) { delete fp; return nullptr; }
        return fp;
    }
    size_t read(void* f, void* buf, size_t len) override {
        if (!f) return 0;
        UINT br = 0; f_read((FIL*)f, buf, (UINT)len, &br); return br;
    }
    size_t write(void* f, const void* buf, size_t len) override {
        if (!f) return 0;
        UINT bw = 0; f_write((FIL*)f, buf, (UINT)len, &bw); return bw;
    }
    bool seek(void* f, long offset, int whence) override {
        if (!f) return false;
        FIL* fp = (FIL*)f;
        FSIZE_t base = (whence == SEEK_CUR) ? f_tell(fp)
                      : (whence == SEEK_END) ? f_size(fp) : 0;
        return f_lseek(fp, base + offset) == FR_OK;
    }
    long tell(void* f) override { return f ? (long)f_tell((FIL*)f) : -1; }
    void close(void* f) override { if (f) { f_close((FIL*)f); delete (FIL*)f; } }

    void* opendir(const char* path) override {
        if (!mounted_) return nullptr;
        FF_DIR* dp = new FF_DIR();
        if (f_opendir(dp, mapPath(path).c_str()) != FR_OK) { delete dp; return nullptr; }
        return dp;
    }
    bool readdir(void* d, FsDirEntry* entry) override {
        if (!d) return false;
        FILINFO fno;
        if (f_readdir((FF_DIR*)d, &fno) != FR_OK || fno.fname[0] == 0) return false;
        strlcpy(entry->name, fno.fname, sizeof(entry->name));
        entry->size = (uint32_t)fno.fsize;
        entry->is_dir = (fno.fattrib & AM_DIR) != 0;
        return true;
    }
    void closedir(void* d) override { if (d) { f_closedir((FF_DIR*)d); delete (FF_DIR*)d; } }

    bool stat(const char* path, uint32_t* size_out, bool* is_dir_out) override {
        if (!mounted_) return false;
        FILINFO fno;
        if (f_stat(mapPath(path).c_str(), &fno) != FR_OK) return false;
        if (size_out) *size_out = (uint32_t)fno.fsize;
        if (is_dir_out) *is_dir_out = (fno.fattrib & AM_DIR) != 0;
        return true;
    }
    bool mkdir(const char* path) override {
        if (!mounted_) return false;
        FRESULT fr = f_mkdir(mapPath(path).c_str());
        return fr == FR_OK || fr == FR_EXIST;
    }
    bool rmdir(const char* path) override {
        if (!mounted_) return false;
        return f_unlink(mapPath(path).c_str()) == FR_OK;
    }
    bool remove(const char* path) override {
        if (!mounted_) return false;
        return f_unlink(mapPath(path).c_str()) == FR_OK;
    }
    bool exists(const char* path) override {
        if (!mounted_) return false;
        FILINFO fno;
        return f_stat(mapPath(path).c_str(), &fno) == FR_OK;
    }

private:
    // FakeSd-backed SdDiskOps (same hooks the firmware backs with sdmmc).
    SdDiskOps makeOps() {
        SdDiskOps ops = {};
        ops.registerRaw    = [](void* c){ ((FakeSd*)c)->register_raw(SD_PDRV_RAW); };
        ops.registerOffset = [](int pdrv, uint32_t s, uint32_t n, void* c){
            ((FakeSd*)c)->register_offset((BYTE)pdrv, s, n); };
        ops.unregister     = [](int pdrv, void* /*c*/){ FakeSd::unregister((BYTE)pdrv); };
        ops.readMbr        = [](uint8_t* b, void* c){ ((FakeSd*)c)->read_sector(0, b); return true; };
        ops.writeMbr       = [](const uint8_t* b, void* c){ ((FakeSd*)c)->write(b, 0, 1); return true; };
        ops.ctx            = disk_;
        return ops;
    }

    bool mountBoth() {
        disk_->register_offset(SD_PDRV_P1, p1Start_, p1Sectors_);
        disk_->register_offset(SD_PDRV_P2, p2Start_, p2Size_);
        if (f_mount(&fsP1_, "2:", 1) != FR_OK) { mounted_ = false; return false; }
        if (f_mount(&fsP2_, "1:", 1) != FR_OK) { f_mount(nullptr, "2:", 0); mounted_ = false; return false; }
        mounted_ = true;
        f_mkdir("1:/upload");
        f_mkdir("1:/logs");
        return true;
    }

    void unmountAll() {
        if (mounted_) { f_mount(nullptr, "2:", 0); f_mount(nullptr, "1:", 0); }
        mounted_ = false;
    }

    static BYTE parseMode(const char* mode) {
        if (!mode || !mode[0]) return 0;
        bool plus = strchr(mode, '+') != nullptr;
        switch (mode[0]) {
            case 'r': return plus ? (FA_READ | FA_WRITE) : FA_READ;
            case 'w': return plus ? (FA_CREATE_ALWAYS | FA_WRITE | FA_READ)
                                  : (FA_CREATE_ALWAYS | FA_WRITE);
            case 'a': return plus ? (FA_OPEN_APPEND | FA_WRITE | FA_READ)
                                  : (FA_OPEN_APPEND | FA_WRITE);
            default:  return 0;
        }
    }

    // Map a HAL path to a FatFs path. P2's prefix is a superstring of P1's
    // (./emu_sdcard_internal vs ./emu_sdcard), so match the longer (P2) first.
    std::string mapPath(const char* path) {
        std::string s(path ? path : "");
        const char* vol; std::string rel;
        if (startsWith(s, p2_))      { vol = "1:"; rel = s.substr(p2_.size()); }  // P2 = internal
        else if (startsWith(s, p1_)) { vol = "2:"; rel = s.substr(p1_.size()); }  // P1 = DSU-facing
        else                         { vol = "1:"; rel = s; if (!rel.empty() && rel[0] != '/') rel = "/" + rel; }
        std::string out = vol;
        if (rel.empty() || rel[0] != '/') out += "/";
        out += rel;
        return out;
    }
    static bool startsWith(const std::string& s, const std::string& p) {
        return !p.empty() && s.compare(0, p.size(), p) == 0;
    }

    // Recursively copy hostDir -> halPrefix (a HAL path) into the card.
    int importRec(const std::string& hostDir, const std::string& halPrefix) {
        DIR* d = ::opendir(hostDir.c_str());
        if (!d) return 0;
        mkdir(halPrefix.c_str());
        int count = 0;
        struct dirent* ent;
        while ((ent = ::readdir(d)) != nullptr) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            std::string hostPath = hostDir + "/" + ent->d_name;
            std::string halPath  = halPrefix + "/" + ent->d_name;
            struct ::stat st;
            if (::stat(hostPath.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) {
                count += importRec(hostPath, halPath);
            } else if (S_ISREG(st.st_mode)) {
                FILE* in = ::fopen(hostPath.c_str(), "rb");
                if (!in) continue;
                void* out = open(halPath.c_str(), "wb");
                if (out) {
                    char buf[2048]; size_t n;
                    while ((n = ::fread(buf, 1, sizeof(buf), in)) > 0) write(out, buf, n);
                    close(out);
                    count++;
                }
                ::fclose(in);
            }
        }
        ::closedir(d);
        return count;
    }

    FakeSd*  disk_ = nullptr;
    FATFS    fsP1_, fsP2_;
    uint32_t cardSectors_ = 0, p1Sectors_ = 0;
    uint32_t p1Start_ = 0, p2Start_ = 0, p2Size_ = 0;
    bool     mounted_ = false;
    std::string p1_, p2_;
};

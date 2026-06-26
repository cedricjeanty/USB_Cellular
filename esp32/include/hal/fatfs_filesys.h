#pragma once
// FatFsFilesys — an IFilesys backed by REAL FatFs running on an in-memory
// FakeSd block device. This is the emulator's "SD through FakeSd": file/dir
// operations execute the same FatFs code (f_open/f_read/f_mkdir/f_mkfs/…) the
// device runs, so a corrupt FAT can be injected at the sector level and the
// firmware's degrade→reformat recovery exercised end-to-end (not just in unit
// tests).
//
// Layout: a single FAT32 volume (drive "0:") on the FakeSd. The two logical
// partitions the firmware uses — P1 (DSU-facing) and P2 (internal) — are mapped
// to subdirectories "0:/p1" and "0:/p2" by HAL path prefix, which is enough to
// run the harvest/upload/log pipeline and the corruption/reformat lifecycle.
//
// Corruption model: corruptFat() zeroes the VBR+FAT head (real sectors) and
// drops the mount, so subsequent ops fail and a remount fails — exactly a
// card whose filesystem is unreadable. reformat() re-runs f_mkfs, which rewrites
// the VBR/FAT and recovers a mountable, writable volume (data lost — the DSU
// re-sends via the cookie), mirroring the device's reformat path.
//
// Single-threaded use assumed (the emulator serializes SD access under the HAL
// lock), matching FatFs's static-LFN-buffer config.

#include "hal/filesys.h"
#include "hal/fake_sd.h"
#include "ff.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>

class FatFsFilesys : public IFilesys {
public:
    FatFsFilesys(uint32_t cardSectors, const char* p1Prefix, const char* p2Prefix)
        : p1_(p1Prefix), p2_(p2Prefix) {
        disk_ = new FakeSd(cardSectors);
    }
    ~FatFsFilesys() { if (mounted_) f_mount(nullptr, "0:", 0); FakeSd::unregister(0); delete disk_; }

    bool mounted() const { return mounted_; }

    // (Re)create the filesystem: mkfs the whole card as one FAT32 volume, mount,
    // and create the p1/p2 base dirs. Returns true on success.
    bool format() {
        if (mounted_) { f_mount(nullptr, "0:", 0); mounted_ = false; }
        FakeSd::unregister(0);
        disk_->register_raw(0);
        static BYTE work[4096];
        // FM_ANY lets FatFs pick FAT12/16/32 + cluster size for the (emulator-
        // sized) card; FM_SFD = no partition table, mkfs writes the VBR at LBA 0.
        MKFS_PARM opt = { (BYTE)(FM_ANY | FM_SFD), 1, 0, 0, 0 };
        if (f_mkfs("0:", &opt, work, sizeof(work)) != FR_OK) return false;
        if (f_mount(&fs_, "0:", 1) != FR_OK) return false;
        mounted_ = true;
        f_mkdir("0:/p1");
        f_mkdir("0:/p2");
        return true;
    }

    // Attempt to (re)mount an existing volume — used by the health/recovery loop.
    bool remount() {
        if (mounted_) return true;
        FakeSd::unregister(0);
        disk_->register_raw(0);
        if (f_mount(&fs_, "0:", 1) != FR_OK) return false;
        mounted_ = true;
        return true;
    }

    // Inject real FAT corruption: zero the VBR + FAT head and drop the mount.
    // Subsequent ops fail and remount() fails until reformat().
    void corruptFat() {
        uint8_t zero[512] = {0};
        for (uint32_t s = 0; s < 64; s++) disk_->write(zero, s, 1);
        if (mounted_) { f_mount(nullptr, "0:", 0); mounted_ = false; }
    }

    // Light health probe (mirrors sd_health_check's f_stat): true if the FS
    // responds. When unmounted, tries a remount first.
    bool probeOk() {
        if (!mounted_ && !remount()) return false;
        FILINFO fno;
        FRESULT fr = f_stat("0:/p2", &fno);
        // FR_OK / FR_NO_FILE = filesystem responded; a dead FS surfaces as
        // FR_DISK_ERR / FR_NO_FILESYSTEM / FR_INT_ERR.
        return (fr == FR_OK || fr == FR_NO_FILE);
    }

    // Copy a host directory tree into the FAT volume under the given HAL prefix,
    // so the emulator's "drop files into ./emu_sdcard" workflow still seeds the
    // in-memory card at boot. Recursive; best-effort; returns the file count.
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
        std::string p = mapPath(path);
        FRESULT fr = f_mkdir(p.c_str());
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

    // Map a HAL path ("./emu_sdcard[_internal]/x") to a FatFs path
    // ("0:/p1/x" or "0:/p2/x"). p2's prefix is a superstring of p1's, so match
    // the longer (p2) first.
    std::string mapPath(const char* path) {
        std::string s(path ? path : "");
        std::string rel; const char* vol = "0:/p1";
        if (startsWith(s, p2_))      { vol = "0:/p2"; rel = s.substr(p2_.size()); }
        else if (startsWith(s, p1_)) { vol = "0:/p1"; rel = s.substr(p1_.size()); }
        else                         { rel = s; if (!rel.empty() && rel[0] != '/') rel = "/" + rel; vol = "0:"; }
        std::string out = vol;
        if (!rel.empty() && rel[0] != '/') out += "/";
        out += rel;
        return out;
    }
    static bool startsWith(const std::string& s, const std::string& p) {
        return !p.empty() && s.compare(0, p.size(), p) == 0;
    }

    // Recursively copy hostDir → halPrefix (a HAL path) into the FAT volume.
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

    FakeSd* disk_ = nullptr;
    FATFS   fs_;
    bool    mounted_ = false;
    std::string p1_, p2_;
};

#pragma once
// AirBridge Filesystem HAL — POSIX-like file and directory operations
// ESP32: wraps VFS/FatFS on SD card. Native: wraps host OS filesystem.

#include <cstdint>
#include <cstddef>

struct FsDirEntry {
    char name[128];
    uint32_t size;
    bool is_dir;
};

class IFilesys {
public:
    virtual ~IFilesys() = default;

    // File operations (stdio-like)
    virtual void* open(const char* path, const char* mode) = 0;
    virtual size_t read(void* f, void* buf, size_t len) = 0;
    virtual size_t write(void* f, const void* buf, size_t len) = 0;
    virtual bool   seek(void* f, long offset, int whence) = 0;
    virtual long   tell(void* f) = 0;
    virtual void   close(void* f) = 0;

    // Directory operations
    virtual void*  opendir(const char* path) = 0;
    virtual bool   readdir(void* d, FsDirEntry* entry) = 0;
    virtual void   closedir(void* d) = 0;

    // Metadata
    virtual bool   stat(const char* path, uint32_t* size_out, bool* is_dir_out) = 0;
    virtual bool   mkdir(const char* path) = 0;
    virtual bool   remove(const char* path) = 0;
    virtual bool   exists(const char* path) = 0;
};

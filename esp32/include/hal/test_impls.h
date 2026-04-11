#pragma once
// Shared test/mock HAL implementations — used by all native tests and the emulator.
// No duplication: every test file and the emulator #include this instead of defining stubs.

#include "hal/hal.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <map>
#include <vector>

// ── Display ─────────────────────────────────────────────────────────────────

// Full test display with framebuffer inspection
class TestDisplay : public IDisplay {
public:
    bool init() override { ok_ = true; clear(); return true; }
    void flush() override { flush_count++; }
    bool ok() const override { return ok_; }

    int flush_count = 0;

    bool pixel_at(int x, int y) const {
        if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return false;
        return (framebuf[x + (y / 8) * SCREEN_W] & (1 << (y & 7))) != 0;
    }

    int pixel_count() const {
        int count = 0;
        for (int i = 0; i < FRAMEBUF_SIZE; i++)
            for (int b = 0; b < 8; b++)
                if (framebuf[i] & (1 << b)) count++;
        return count;
    }

    void reset() { clear(); flush_count = 0; }

private:
    bool ok_ = false;
};

// Minimal stub when display isn't relevant to the test
class StubDisplay : public IDisplay {
public:
    bool init() override { ok_ = true; return true; }
    void flush() override {}
    bool ok() const override { return ok_; }
private:
    bool ok_ = false;
};

// ── Clock ───────────────────────────────────────────────────────────────────

// Simulated clock with manual time control
class TestClock : public IClock {
public:
    uint32_t now_ms = 0;
    uint32_t millis() override { return now_ms; }
    void delay_ms(uint32_t ms) override { now_ms += ms; }
    void advance(uint32_t ms) { now_ms += ms; }
};

// Minimal stub
class StubClock : public IClock {
public:
    uint32_t millis() override { return 0; }
    void delay_ms(uint32_t) override {}
};

// ── NVS ─────────────────────────────────────────────────────────────────────

// In-memory key-value store
class TestNvs : public INvs {
public:
    std::map<std::string, std::map<std::string, std::string>> store;

    void clear_all() { store.clear(); }

    bool get_str(const char* ns, const char* key, char* out, size_t sz) override {
        auto nit = store.find(ns);
        if (nit == store.end()) { out[0] = '\0'; return false; }
        auto kit = nit->second.find(key);
        if (kit == nit->second.end()) { out[0] = '\0'; return false; }
        strlcpy(out, kit->second.c_str(), sz);
        return true;
    }
    bool set_str(const char* ns, const char* key, const char* val) override {
        store[ns][key] = val; return true;
    }
    bool get_u8(const char* ns, const char* key, uint8_t* out) override {
        auto nit = store.find(ns); if (nit == store.end()) return false;
        auto kit = nit->second.find(key); if (kit == nit->second.end()) return false;
        *out = (uint8_t)std::stoi(kit->second); return true;
    }
    bool set_u8(const char* ns, const char* key, uint8_t val) override {
        store[ns][key] = std::to_string(val); return true;
    }
    bool get_i32(const char* ns, const char* key, int32_t* out) override {
        auto nit = store.find(ns); if (nit == store.end()) return false;
        auto kit = nit->second.find(key); if (kit == nit->second.end()) return false;
        *out = (int32_t)std::stol(kit->second); return true;
    }
    bool set_i32(const char* ns, const char* key, int32_t val) override {
        store[ns][key] = std::to_string(val); return true;
    }
    bool get_u32(const char* ns, const char* key, uint32_t* out) override {
        auto nit = store.find(ns); if (nit == store.end()) return false;
        auto kit = nit->second.find(key); if (kit == nit->second.end()) return false;
        *out = (uint32_t)std::stoul(kit->second); return true;
    }
    bool set_u32(const char* ns, const char* key, uint32_t val) override {
        store[ns][key] = std::to_string(val); return true;
    }
    void erase_key(const char* ns, const char* key) override {
        auto nit = store.find(ns);
        if (nit != store.end()) nit->second.erase(key);
    }
};

// Minimal stub (all ops return false/empty)
class StubNvs : public INvs {
public:
    bool get_str(const char*, const char*, char* o, size_t) override { o[0]=0; return false; }
    bool set_str(const char*, const char*, const char*) override { return true; }
    bool get_u8(const char*, const char*, uint8_t*) override { return false; }
    bool set_u8(const char*, const char*, uint8_t) override { return true; }
    bool get_i32(const char*, const char*, int32_t*) override { return false; }
    bool set_i32(const char*, const char*, int32_t) override { return true; }
    bool get_u32(const char*, const char*, uint32_t*) override { return false; }
    bool set_u32(const char*, const char*, uint32_t) override { return true; }
    void erase_key(const char*, const char*) override {}
};

// ── Filesystem ──────────────────────────────────────────────────────────────

// In-memory filesystem for testing
class MemFilesys : public IFilesys {
public:
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<std::string, bool> dirs;  // tracks directories

    void clear_all() { files.clear(); dirs.clear(); }

    void add_file(const char* path, const void* data, size_t len) {
        files[path].assign((const uint8_t*)data, (const uint8_t*)data + len);
    }
    void add_file_str(const char* path, const char* content) {
        add_file(path, content, strlen(content));
    }
    void add_dir(const char* path) { dirs[path] = true; }

    bool has_file(const char* path) { return files.count(path) && !dirs.count(path); }
    std::string get_content(const char* path) {
        auto it = files.find(path);
        if (it == files.end()) return "";
        return std::string(it->second.begin(), it->second.end());
    }

    // -- IFilesys implementation --
    struct MemHandle { std::string path; size_t pos; std::string mode; std::vector<uint8_t> wbuf; };

    void* open(const char* path, const char* mode) override {
        if (mode[0] == 'r') {
            auto it = files.find(path);
            if (it == files.end()) return nullptr;
        }
        return new MemHandle{path, 0, mode, {}};
    }
    size_t read(void* f, void* buf, size_t len) override {
        auto* h = (MemHandle*)f;
        auto it = files.find(h->path);
        if (it == files.end()) return 0;
        size_t avail = it->second.size() - h->pos;
        size_t n = (len < avail) ? len : avail;
        memcpy(buf, it->second.data() + h->pos, n);
        h->pos += n;
        return n;
    }
    size_t write(void* f, const void* buf, size_t len) override {
        auto* h = (MemHandle*)f;
        h->wbuf.insert(h->wbuf.end(), (const uint8_t*)buf, (const uint8_t*)buf + len);
        return len;
    }
    bool seek(void* f, long offset, int whence) override {
        auto* h = (MemHandle*)f;
        if (whence == 0 /*SEEK_SET*/) h->pos = offset;
        else if (whence == 2 /*SEEK_END*/) {
            auto it = files.find(h->path);
            if (it != files.end()) h->pos = it->second.size() + offset;
        }
        return true;
    }
    long tell(void* f) override { return ((MemHandle*)f)->pos; }
    void close(void* f) override {
        auto* h = (MemHandle*)f;
        if (!h->wbuf.empty()) {
            files[h->path] = h->wbuf;
        }
        delete h;
    }

    struct DirIter { std::string prefix; std::vector<std::string> entries; size_t idx; };
    void* opendir(const char* path) override {
        std::string pfx = std::string(path) + "/";
        auto* d = new DirIter{pfx, {}, 0};
        for (auto& kv : files) {
            if (kv.first.substr(0, pfx.size()) == pfx) {
                std::string rest = kv.first.substr(pfx.size());
                if (rest.find('/') == std::string::npos) d->entries.push_back(rest);
            }
        }
        for (auto& kv : dirs) {
            if (kv.first.substr(0, pfx.size()) == pfx) {
                std::string rest = kv.first.substr(pfx.size());
                if (!rest.empty() && rest.find('/') == std::string::npos) d->entries.push_back(rest);
            }
        }
        return d;
    }
    bool readdir(void* dir, FsDirEntry* entry) override {
        auto* d = (DirIter*)dir;
        if (d->idx >= d->entries.size()) return false;
        std::string name = d->entries[d->idx++];
        strlcpy(entry->name, name.c_str(), sizeof(entry->name));
        std::string full = d->prefix + name;
        entry->is_dir = dirs.count(full) > 0;
        auto fit = files.find(full);
        entry->size = (fit != files.end()) ? fit->second.size() : 0;
        return true;
    }
    void closedir(void* d) override { delete (DirIter*)d; }

    bool stat(const char* path, uint32_t* size_out, bool* is_dir_out) override {
        if (dirs.count(path)) {
            if (size_out) *size_out = 0;
            if (is_dir_out) *is_dir_out = true;
            return true;
        }
        auto it = files.find(path);
        if (it == files.end()) return false;
        if (size_out) *size_out = it->second.size();
        if (is_dir_out) *is_dir_out = false;
        return true;
    }
    bool mkdir(const char* path) override { dirs[path] = true; return true; }
    bool remove(const char* path) override { return files.erase(path) > 0; }
    bool exists(const char* path) override { return files.count(path) > 0 || dirs.count(path) > 0; }
};

// Minimal stub
class StubFilesys : public IFilesys {
public:
    void* open(const char*, const char*) override { return nullptr; }
    size_t read(void*, void*, size_t) override { return 0; }
    size_t write(void*, const void*, size_t) override { return 0; }
    bool seek(void*, long, int) override { return false; }
    long tell(void*) override { return 0; }
    void close(void*) override {}
    void* opendir(const char*) override { return nullptr; }
    bool readdir(void*, FsDirEntry*) override { return false; }
    void closedir(void*) override {}
    bool stat(const char*, uint32_t*, bool*) override { return false; }
    bool mkdir(const char*) override { return false; }
    bool remove(const char*) override { return false; }
    bool exists(const char*) override { return false; }
};

// ── Network ─────────────────────────────────────────────────────────────────

// Mock network with canned HTTP responses
class MockNetwork : public INetwork {
public:
    struct MockConn { std::string resp; size_t pos; std::string captured; };
    std::string next_response;
    std::string last_host;
    std::string last_request;
    bool fail_connect = false;

    void reset() { next_response.clear(); last_host.clear(); last_request.clear(); fail_connect = false; }

    TlsHandle connect(const char* host) override {
        if (fail_connect) return nullptr;
        last_host = host;
        return new MockConn{next_response, 0, ""};
    }
    bool write(TlsHandle conn, const void* data, size_t len) override {
        auto* c = (MockConn*)conn;
        c->captured.append((const char*)data, len);
        last_request = c->captured;
        return true;
    }
    int read(TlsHandle conn, void* buf, size_t len) override {
        auto* c = (MockConn*)conn;
        size_t avail = c->resp.size() - c->pos;
        if (avail == 0) return 0;
        size_t n = (len < avail) ? len : avail;
        memcpy(buf, c->resp.c_str() + c->pos, n);
        c->pos += n;
        return (int)n;
    }
    void destroy(TlsHandle conn) override { delete (MockConn*)conn; }
};

// Minimal stub
class StubNetwork : public INetwork {
public:
    TlsHandle connect(const char*) override { return nullptr; }
    bool write(TlsHandle, const void*, size_t) override { return false; }
    int read(TlsHandle, void*, size_t) override { return 0; }
    void destroy(TlsHandle) override {}
};

#pragma once
// Native HAL implementations — real I/O on the host OS.
// Used by the emulator. Not for unit tests (use test_impls.h instead).

#if !defined(ESP_PLATFORM)

#include "hal/hal.h"
#include "hal/net_util.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <mutex>
#include <atomic>
#include <unistd.h>

// ── Simulated shared SD/SPI peripheral ──────────────────────────────────────
// On hardware the SD card sits on ONE SPI bus shared between the firmware's
// FATFS reads and the USB MSC raw-sector reads, serialized by a single mutex
// (g_sd_mutex). The native HAL normally has no such contention (fast host FS,
// no-op lock). When the emulator installs a SimSdCard, NativeFilesys routes its
// lock()/unlock() through this single timed_mutex and adds per-read SPI latency,
// so a large upload can be made to contend with a simulated MSC reader exactly
// like the device. Left null (default) for unit tests → no latency/contention.
struct SimSdCard {
    std::timed_mutex bus;             // the single SPI/SD access lock (== g_sd_mutex)
    int  readKBps = 0;                // 0 = instant; else per-read latency modeling SPI throughput
    std::atomic<long> mscReads{0};    // simulated MSC sector reads that got the bus
    std::atomic<long> mscTimeouts{0}; // simulated MSC reads that timed out (host sees -1)
    std::atomic<long> fsReads{0};     // firmware/FATFS reads served
};
inline SimSdCard*& g_simSdCard() { static SimSdCard* p = nullptr; return p; }

// ── File-backed NVS ─────────────────────────────────────────────────────────

class FileNvs : public INvs {
public:
    std::map<std::string, std::map<std::string, std::string>> store;
    std::string path;

    FileNvs(const char* filepath) : path(filepath) { load(); }

    void load() {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char ns[64], key[64], val[384];
            if (sscanf(line, "%63[^:]:%63[^=]=%383[^\n]", ns, key, val) == 3)
                store[ns][key] = val;
        }
        fclose(f);
    }

    void save() {
        FILE* f = fopen(path.c_str(), "w");
        if (!f) return;
        for (auto& ns : store)
            for (auto& kv : ns.second)
                fprintf(f, "%s:%s=%s\n", ns.first.c_str(), kv.first.c_str(), kv.second.c_str());
        fclose(f);
    }

    bool get_str(const char* ns, const char* key, char* out, size_t sz) override {
        auto nit = store.find(ns);
        if (nit == store.end()) { out[0] = '\0'; return false; }
        auto kit = nit->second.find(key);
        if (kit == nit->second.end()) { out[0] = '\0'; return false; }
        strlcpy(out, kit->second.c_str(), sz);
        return true;
    }
    bool set_str(const char* ns, const char* key, const char* val) override {
        store[ns][key] = val; save(); return true;
    }
    bool get_u8(const char* ns, const char* key, uint8_t* out) override {
        auto nit = store.find(ns); if (nit == store.end()) return false;
        auto kit = nit->second.find(key); if (kit == nit->second.end()) return false;
        *out = (uint8_t)std::stoi(kit->second); return true;
    }
    bool set_u8(const char* ns, const char* key, uint8_t val) override {
        store[ns][key] = std::to_string(val); save(); return true;
    }
    bool get_i32(const char* ns, const char* key, int32_t* out) override {
        auto nit = store.find(ns); if (nit == store.end()) return false;
        auto kit = nit->second.find(key); if (kit == nit->second.end()) return false;
        *out = (int32_t)std::stol(kit->second); return true;
    }
    bool set_i32(const char* ns, const char* key, int32_t val) override {
        store[ns][key] = std::to_string(val); save(); return true;
    }
    bool get_u32(const char* ns, const char* key, uint32_t* out) override {
        auto nit = store.find(ns); if (nit == store.end()) return false;
        auto kit = nit->second.find(key); if (kit == nit->second.end()) return false;
        *out = (uint32_t)std::stoul(kit->second); return true;
    }
    bool set_u32(const char* ns, const char* key, uint32_t val) override {
        store[ns][key] = std::to_string(val); save(); return true;
    }
    void erase_key(const char* ns, const char* key) override {
        auto nit = store.find(ns);
        if (nit != store.end()) { nit->second.erase(key); save(); }
    }
};

// ── Host OS filesystem ──────────────────────────────────────────────────────

class NativeFilesys : public IFilesys {
public:
    void* open(const char* path, const char* mode) override { return (void*)fopen(path, mode); }
    size_t read(void* f, void* buf, size_t len) override {
        size_t n = fread(buf, 1, len, (FILE*)f);
        // Model SD/SPI read throughput + count FATFS reads (only when contention installed).
        if (SimSdCard* sd = g_simSdCard()) {
            sd->fsReads++;
            if (sd->readKBps > 0 && n > 0) usleep((useconds_t)(n * 1000ULL / sd->readKBps));
        }
        return n;
    }
    size_t write(void* f, const void* buf, size_t len) override { return fwrite(buf, 1, len, (FILE*)f); }
    // SD bus lock — serializes against the simulated MSC reader, like g_sd_mutex.
    void lock() override   { if (SimSdCard* sd = g_simSdCard()) sd->bus.lock(); }
    void unlock() override { if (SimSdCard* sd = g_simSdCard()) sd->bus.unlock(); }
    bool seek(void* f, long offset, int whence) override { return fseek((FILE*)f, offset, whence) == 0; }
    long tell(void* f) override { return ftell((FILE*)f); }
    void close(void* f) override { if (f) fclose((FILE*)f); }

    void* opendir(const char* path) override { return (void*)::opendir(path); }
    bool readdir(void* d, FsDirEntry* entry) override {
        struct dirent* ent = ::readdir((DIR*)d);
        if (!ent) return false;
        strlcpy(entry->name, ent->d_name, sizeof(entry->name));
        entry->size = 0;
        entry->is_dir = (ent->d_type == DT_DIR);
        return true;
    }
    void closedir(void* d) override { ::closedir((DIR*)d); }

    bool stat(const char* path, uint32_t* size_out, bool* is_dir_out) override {
        struct ::stat st;
        if (::stat(path, &st) != 0) return false;
        if (size_out) *size_out = (uint32_t)st.st_size;
        if (is_dir_out) *is_dir_out = S_ISDIR(st.st_mode);
        return true;
    }
    bool mkdir(const char* path) override { return ::mkdir(path, 0755) == 0 || errno == EEXIST; }
    bool rmdir(const char* path) override { return ::rmdir(path) == 0; }
    bool remove(const char* path) override { return ::remove(path) == 0; }
    bool exists(const char* path) override {
        struct ::stat st;
        return ::stat(path, &st) == 0;
    }
};

// ── OpenSSL TLS network ─────────────────────────────────────────────────────

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

class OpenSSLNetwork : public INetwork {
public:
    char bindAddr[32] = "";    // bind to specific source IP (e.g. PPP interface)
    int maxBytesPerSec = 0;    // bandwidth limit (0 = unlimited)

    // Fault injection (tests): if >0, the Nth S3 part PUT (1-indexed, by PUT
    // connection order) still uploads its body to S3 but its RESPONSE read
    // returns 0 (EOF) — simulating a response lost on a flaky link. The firmware
    // sees no ETag and retries the part; the retry is a new PUT connection with a
    // higher index, so it reads the ETag normally. Models "PUT succeeded but the
    // ack didn't come back," the exact case the part-PUT retry guards against.
    int dropPutResponseOnPart = 0;

    OpenSSLNetwork() {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
        ctx_ = SSL_CTX_new(TLS_client_method());
        SSL_CTX_set_default_verify_paths(ctx_);
    }
    ~OpenSSLNetwork() {
        if (ctx_) SSL_CTX_free(ctx_);
    }

    struct Conn { SSL* ssl; int fd; bool sawPut = false; bool dropResp = false; };
    int putCount_ = 0;  // count of PUT connections seen (for fault injection)

    TlsHandle connect(const char* host) override {
        struct addrinfo hints = {}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host, "443", &hints, &res) != 0) return nullptr;

        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); return nullptr; }

        // Bind to specific source IP if configured (e.g. PPP interface)
        if (bindAddr[0]) {
            struct sockaddr_in src = {};
            src.sin_family = AF_INET;
            src.sin_addr.s_addr = inet_addr(bindAddr);
            bind(fd, (struct sockaddr*)&src, sizeof(src));
        }

        if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            ::close(fd); freeaddrinfo(res); return nullptr;
        }
        freeaddrinfo(res);

        SSL* ssl = SSL_new(ctx_);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); ::close(fd); return nullptr;
        }
        return (TlsHandle)new Conn{ssl, fd};
    }

    // Non-blocking primitive for netWriteAll(). SSL sockets here are blocking, so SSL_write
    // returns >0 or <=0(error) and never WANT_WRITE — but map it the same way for the shared
    // helper (>0 bytes, 0 would-block, <0 error).
    int writeSome(TlsHandle conn, const void* data, size_t len) override {
        auto* c = (Conn*)conn;
        int w = SSL_write(c->ssl, data, len);
        if (w > 0) return w;
        int err = SSL_get_error(c->ssl, w);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) return 0;
        return -1;
    }
    bool write(TlsHandle conn, const void* data, size_t len) override {
        auto* c = (Conn*)conn;
        // Detect a part PUT (request line starts with "PUT ") on its first write,
        // and decide whether to drop this connection's response (fault injection).
        if (!c->sawPut && len >= 4 && memcmp(data, "PUT ", 4) == 0) {
            c->sawPut = true;
            putCount_++;
            if (dropPutResponseOnPart > 0 && putCount_ == dropPutResponseOnPart)
                c->dropResp = true;
        }
        return netWriteAll(this, conn, data, len);
    }

    int read(TlsHandle conn, void* buf, size_t len) override {
        auto* c = (Conn*)conn;
        if (c->dropResp) return 0;  // simulate lost response: EOF before any data
        int r = SSL_read(c->ssl, buf, len);
        if (r <= 0) return (SSL_get_error(c->ssl, r) == SSL_ERROR_ZERO_RETURN) ? 0 : -1;
        return r;
    }

    void destroy(TlsHandle conn) override {
        auto* c = (Conn*)conn;
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        ::close(c->fd);
        delete c;
    }

    int getMaxBytesPerSec() override { return maxBytesPerSec; }

    // Observable in the emulator: true while a multipart upload session is
    // bracketed. On the firmware this maps to session-wide +++ suppression; here
    // it lets a test/harness confirm the bracket spans the part PUTs (and, once
    // the modem watchdog is shared, that no +++ fires during it).
    bool uploadSessionActive = false;
    int  uploadSessionBegins = 0;
    void setUploadSession(bool active) override {
        uploadSessionActive = active;
        if (active) uploadSessionBegins++;
    }

private:
    SSL_CTX* ctx_ = nullptr;
};

#endif // !ESP_PLATFORM

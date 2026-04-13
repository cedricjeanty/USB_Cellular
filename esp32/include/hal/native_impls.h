#pragma once
// Native HAL implementations — real I/O on the host OS.
// Used by the emulator. Not for unit tests (use test_impls.h instead).

#if !defined(ESP_PLATFORM)

#include "hal/hal.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

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
    size_t read(void* f, void* buf, size_t len) override { return fread(buf, 1, len, (FILE*)f); }
    size_t write(void* f, const void* buf, size_t len) override { return fwrite(buf, 1, len, (FILE*)f); }
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

    struct Conn { SSL* ssl; int fd; };

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

    bool write(TlsHandle conn, const void* data, size_t len) override {
        auto* c = (Conn*)conn;
        const char* p = (const char*)data;
        size_t rem = len;
        while (rem > 0) {
            int w = SSL_write(c->ssl, p, rem);
            if (w <= 0) return false;
            p += w; rem -= w;
        }
        return true;
    }

    int read(TlsHandle conn, void* buf, size_t len) override {
        auto* c = (Conn*)conn;
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

private:
    SSL_CTX* ctx_ = nullptr;
};

#endif // !ESP_PLATFORM

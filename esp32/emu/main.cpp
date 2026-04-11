// AirBridge OLED Emulator — full device emulation with real S3 connectivity
// Build: cd esp32 && pio run -e emulator
// Run:   .pio/build/emulator/program [device_id]
//
// Uses laptop internet for real S3 uploads via the Lambda API Gateway.
// Virtual SD card at ./emu_sdcard/ — drop files there to simulate USB writes.
// NVS state persisted to ./emu_nvs.dat
//
// Keyboard:
//   C       Toggle cellular connected (enables/disables uploads)
//   H       Harvest files from emu_sdcard/ to emu_sdcard/harvested/
//   P       Upload harvested files to S3 (requires C first)
//   U       Simulate USB write (+10 MB display counter)
//   +/-     Adjust display upload speed
//   S       Step upload progress (+5 MB display counter)
//   R       Reset display state
//   Q/Esc   Quit

#include <SDL2/SDL.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <map>
#include <vector>

#include "hal/hal.h"
#include "hal/uart_pty.h"
#include "sim_modem.h"
#include "airbridge_display.h"
#include "airbridge_harvest.h"
#include "airbridge_http.h"

// ── Display HAL ─────────────────────────────────────────────────────────────

class EmuDisplay : public IDisplay {
public:
    bool init() override { ok_ = true; clear(); return true; }
    void flush() override {}
    bool ok() const override { return ok_; }
private:
    bool ok_ = false;
};

// ── Clock HAL ───────────────────────────────────────────────────────────────

class EmuClock : public IClock {
public:
    uint32_t millis() override { return SDL_GetTicks(); }
    void delay_ms(uint32_t ms) override { SDL_Delay(ms); }
};

// ── NVS HAL (file-backed) ───────────────────────────────────────────────────

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

// ── Filesystem HAL (host OS directory) ──────────────────────────────────────

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
        // Get full info via stat in the caller's context — set defaults here
        entry->size = 0;
        entry->is_dir = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_REG || ent->d_type == DT_UNKNOWN) {
            entry->is_dir = false;
        }
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
    bool remove(const char* path) override { return ::remove(path) == 0; }
    bool exists(const char* path) override {
        struct ::stat st;
        return ::stat(path, &st) == 0;
    }
};

// ── Network HAL (OpenSSL TLS) ───────────────────────────────────────────────

class OpenSSLNetwork : public INetwork {
public:
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
        if (getaddrinfo(host, "443", &hints, &res) != 0) {
            printf("[NET] DNS failed for %s\n", host);
            return nullptr;
        }

        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) { freeaddrinfo(res); return nullptr; }

        if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            printf("[NET] TCP connect failed to %s\n", host);
            ::close(fd);
            freeaddrinfo(res);
            return nullptr;
        }
        freeaddrinfo(res);

        SSL* ssl = SSL_new(ctx_);
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, host);
        if (SSL_connect(ssl) != 1) {
            printf("[NET] TLS handshake failed to %s\n", host);
            SSL_free(ssl);
            ::close(fd);
            return nullptr;
        }

        auto* c = new Conn{ssl, fd};
        return (TlsHandle)c;
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
        if (r <= 0) {
            int err = SSL_get_error(c->ssl, r);
            if (err == SSL_ERROR_ZERO_RETURN) return 0;
            return -1;
        }
        return r;
    }

    void destroy(TlsHandle conn) override {
        auto* c = (Conn*)conn;
        SSL_shutdown(c->ssl);
        SSL_free(c->ssl);
        ::close(c->fd);
        delete c;
    }

private:
    SSL_CTX* ctx_ = nullptr;
};

// ── HAL instances ───────────────────────────────────────────────────────────

static EmuDisplay     s_display;
static EmuClock       s_clock;
static FileNvs        s_nvs("./emu_nvs.dat");
static NativeFilesys  s_fs;
static OpenSSLNetwork s_net;
static PtyUart        s_uart;
static HAL            s_hal = { &s_display, &s_clock, &s_nvs, &s_fs, &s_net, &s_uart };
HAL* g_hal = &s_hal;

static SimModem* s_modem = nullptr;

static const char* SD_ROOT = "./emu_sdcard";

// ── S3 upload — uses shared halS3UploadFile() from airbridge_s3.h ───────────

#include "airbridge_s3.h"

static void doUpload(DisplayState& ds) {
    char harvestDir[256];
    snprintf(harvestDir, sizeof(harvestDir), "%s/harvested", SD_ROOT);

    // Scan for files needing upload (skip .done__ markers)
    void* dir = g_hal->filesys->opendir(harvestDir);
    if (!dir) { printf("[S3] No harvested directory\n"); return; }

    std::vector<std::string> files;
    FsDirEntry ent;
    while (g_hal->filesys->readdir(dir, &ent)) {
        if (ent.name[0] == '.') continue;

        char donePath[512];
        snprintf(donePath, sizeof(donePath), "%s/.done__%s", harvestDir, ent.name);
        if (g_hal->filesys->exists(donePath)) continue;

        files.push_back(ent.name);
    }
    g_hal->filesys->closedir(dir);

    printf("[S3] %zu file(s) to upload\n", files.size());
    for (auto& name : files) {
        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", harvestDir, name.c_str());
        float fileMb = 0;
        { uint32_t sz = 0; bool d = false; g_hal->filesys->stat(fullpath, &sz, &d); fileMb = sz / 1e6f; }

        printf("[S3] Uploading %s (%.1f MB)...\n", name.c_str(), fileMb);
        UploadResult res = halS3UploadFile(fullpath, name.c_str());

        if (res.success) {
            printf("[S3] Upload complete: %.0f KB/s\n", res.kbps);
            ds.mbUploaded += fileMb;
            ds.uploadKBps = res.kbps;

            // Create .done__ marker
            char donePath[512];
            snprintf(donePath, sizeof(donePath), "%s/harvested/.done__%s", SD_ROOT, name.c_str());
            void* df = g_hal->filesys->open(donePath, "w");
            if (df) g_hal->filesys->close(df);
        } else {
            printf("[S3] Upload failed: %s\n", res.error);
        }
    }
}

// ── SDL2 rendering ──────────────────────────────────────────────────────────

static const int SCALE = 6;
static const int WIN_W = SCREEN_W * SCALE;
static const int WIN_H = SCREEN_H * SCALE;

static void renderFramebuffer(SDL_Renderer* renderer) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            bool on = (s_display.framebuf[x + (y / 8) * SCREEN_W] & (1 << (y & 7))) != 0;
            if (on) {
                SDL_Rect r = { x * SCALE, y * SCALE, SCALE, SCALE };
                SDL_RenderFillRect(renderer, &r);
            }
        }
    }
    SDL_RenderPresent(renderer);
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    s_display.init();

    // Set device ID from command line or default
    const char* deviceId = (argc > 1) ? argv[1] : "EMU000001";
    s_nvs.set_str("s3", "device_id", deviceId);

    // Provision S3 credentials if not already set
    char tmp[4] = "";
    if (!s_nvs.get_str("s3", "api_host", tmp, sizeof(tmp)) || tmp[0] == '\0') {
        s_nvs.set_str("s3", "api_host", "disw6oxjed.execute-api.us-west-2.amazonaws.com");
        s_nvs.set_str("s3", "api_key",  "7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm");
        printf("S3 credentials provisioned (first run)\n");
    }

    // Create virtual SD card directory
    ::mkdir(SD_ROOT, 0755);

    // Start SIM7600 modem simulator
    s_modem = new SimModem();
    s_modem->operatorName = "SimOperator";
    s_modem->rssi = 22;
    s_modem->echoEnabled = false;
    if (s_modem->start()) {
        s_uart.fd = s_modem->slave_fd;
        printf("SIM7600 simulator running on PTY fd=%d\n", s_modem->slave_fd);
    } else {
        printf("WARNING: SIM7600 simulator failed to start\n");
    }

    printf("AirBridge Emulator — device_id=%s\n", deviceId);
    printf("Virtual SD card: %s/\n", SD_ROOT);
    printf("NVS storage:     ./emu_nvs.dat\n\n");
    printf("Keys: C=cellular  H=harvest  P=upload-to-S3  T=test-AT-cmd\n");
    printf("      U=usb-write +/-=speed  S=step  R=reset  Q=quit\n\n");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow(
        "AirBridge OLED Emulator (128x64)",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);

    DisplayState ds = {};
    strlcpy(ds.modemOp, "Emulator", sizeof(ds.modemOp));

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_KEYDOWN) {
                switch (event.key.keysym.sym) {
                case SDLK_q:
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_c:
                    ds.pppConnected = !ds.pppConnected;
                    if (ds.pppConnected) { ds.modemRssi = 22; ds.modemReady = true; }
                    else { ds.modemRssi = 0; }
                    printf("Cellular: %s\n", ds.pppConnected ? "ON (using laptop internet)" : "OFF");
                    break;
                case SDLK_h: {
                    printf("Harvesting from %s/ ...\n", SD_ROOT);
                    char destDir[256];
                    snprintf(destDir, sizeof(destDir), "%s/harvested", SD_ROOT);
                    HarvestResult r = harvestFiles(SD_ROOT, destDir);
                    printf("Harvested: %u file(s), %.1f MB\n", r.count, r.usedMb);
                    ds.mbQueued += r.usedMb;
                    break;
                }
                case SDLK_p:
                    if (!ds.pppConnected) {
                        printf("Press C first to enable network\n");
                    } else {
                        printf("Uploading to S3...\n");
                        doUpload(ds);
                    }
                    break;
                case SDLK_u:
                    ds.hostWrittenMb += 10.0f;
                    ds.usbWriteKBps = 5000.0f;
                    printf("USB write: +10MB\n");
                    break;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                    ds.uploadKBps += 50.0f;
                    break;
                case SDLK_MINUS:
                    ds.uploadKBps = (ds.uploadKBps > 50.0f) ? ds.uploadKBps - 50.0f : 0.0f;
                    break;
                case SDLK_s:
                    ds.mbUploaded += 5.0f;
                    ds.uploadingMb = 2.0f;
                    break;
                case SDLK_r:
                    ds = {};
                    strlcpy(ds.modemOp, "Emulator", sizeof(ds.modemOp));
                    printf("State reset\n");
                    break;
                case SDLK_t: {
                    // Test AT command sequence via simulated UART
                    if (!s_uart.fd || s_uart.fd < 0) {
                        printf("No modem simulator running\n");
                        break;
                    }
                    printf("[AT] Sending init sequence...\n");
                    auto atCmd = [](const char* cmd) {
                        std::string full = std::string(cmd) + "\r";
                        s_uart.write(full.c_str(), full.size());
                        usleep(100000);
                        char buf[256] = "";
                        int total = 0;
                        for (int i = 0; i < 10; i++) {
                            int n = s_uart.read(buf + total, sizeof(buf) - 1 - total, 100);
                            if (n > 0) total += n;
                            buf[total] = '\0';
                            if (strstr(buf, "OK") || strstr(buf, "ERROR") || strstr(buf, "CONNECT")) break;
                        }
                        printf("[AT] %s → %s", cmd, buf);
                        if (buf[total-1] != '\n') printf("\n");
                    };
                    atCmd("AT");
                    atCmd("AT+CSQ");
                    atCmd("AT+COPS?");
                    atCmd("AT+CREG?");
                    atCmd("AT+CCLK?");
                    printf("[AT] Done\n");
                    break;
                }
                default:
                    break;
                }
            }
        }

        if (ds.usbWriteKBps > 0.5f) ds.usbWriteKBps *= 0.95f;

        updateDisplay(ds);
        renderFramebuffer(renderer);
        SDL_Delay(100);
    }

    if (s_modem) { s_modem->stop(); delete s_modem; }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

// AirBridge OLED Emulator — pixel-perfect SDL2 display rendering
// Build: cd esp32 && pio run -e emulator
// Run:   .pio/build/emulator/program
//
// Keyboard:
//   C       Toggle cellular connected (+ signal)
//   W       Toggle WiFi connected
//   M       Toggle modem ready (connecting state)
//   U       Simulate USB write (+10 MB)
//   H       Simulate harvest (queue files for upload)
//   +/-     Adjust upload speed
//   S       Step upload progress (+5 MB uploaded)
//   R       Reset all state
//   Q/Esc   Quit

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include "hal/hal.h"
#include "airbridge_display.h"

// ── Emulator HAL implementations ────────────────────────────────────────────

class EmuDisplay : public IDisplay {
public:
    bool init() override { ok_ = true; clear(); return true; }
    void flush() override { flush_count++; }
    bool ok() const override { return ok_; }
    int flush_count = 0;
private:
    bool ok_ = false;
};

class EmuClock : public IClock {
public:
    uint32_t millis() override { return SDL_GetTicks(); }
    void delay_ms(uint32_t ms) override { SDL_Delay(ms); }
};

// Stubs for unused HAL interfaces
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
class StubNetwork : public INetwork {
public:
    TlsHandle connect(const char*) override { return nullptr; }
    bool write(TlsHandle, const void*, size_t) override { return false; }
    int read(TlsHandle, void*, size_t) override { return 0; }
    void destroy(TlsHandle) override {}
};

static EmuDisplay  s_display;
static EmuClock    s_clock;
static StubNvs     s_nvs;
static StubFilesys s_fs;
static StubNetwork s_net;
static HAL         s_hal = { &s_display, &s_clock, &s_nvs, &s_fs, &s_net };
HAL* g_hal = &s_hal;

// ── SDL2 rendering ──────────────────────────────────────────────────────────

static const int SCALE = 6;
static const int WIN_W = SCREEN_W * SCALE;
static const int WIN_H = SCREEN_H * SCALE;

// Monochrome OLED: white on black
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
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }

    DisplayState ds = {};
    strlcpy(ds.modemOp, "T-Mobile", sizeof(ds.modemOp));

    printf("AirBridge OLED Emulator\n");
    printf("Keys: C=cellular W=wifi M=modem U=usb-write H=harvest\n");
    printf("      +/-=upload-speed S=step-upload R=reset Q=quit\n\n");

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
                    else ds.modemRssi = 0;
                    printf("Cellular: %s (rssi=%d)\n", ds.pppConnected ? "ON" : "OFF", ds.modemRssi);
                    break;
                case SDLK_w:
                    ds.netConnected = !ds.netConnected;
                    if (ds.netConnected) {
                        strlcpy(ds.wifiLabel, "MyWiFi", sizeof(ds.wifiLabel));
                        ds.wifiBars = 3;
                    }
                    printf("WiFi: %s\n", ds.netConnected ? "ON" : "OFF");
                    break;
                case SDLK_m:
                    ds.modemReady = !ds.modemReady;
                    printf("Modem ready: %s\n", ds.modemReady ? "YES" : "NO");
                    break;
                case SDLK_u:
                    ds.hostWrittenMb += 10.0f;
                    ds.usbWriteKBps = 5000.0f;
                    printf("USB write: +10MB (total=%.1fMB, speed=5000KB/s)\n", ds.hostWrittenMb);
                    break;
                case SDLK_h:
                    ds.mbQueued += 50.0f;
                    printf("Harvest: +50MB queued (total=%.1fMB)\n", ds.mbQueued);
                    break;
                case SDLK_EQUALS:
                case SDLK_PLUS:
                    ds.uploadKBps += 50.0f;
                    printf("Upload speed: %.0f KB/s\n", ds.uploadKBps);
                    break;
                case SDLK_MINUS:
                    ds.uploadKBps = (ds.uploadKBps > 50.0f) ? ds.uploadKBps - 50.0f : 0.0f;
                    printf("Upload speed: %.0f KB/s\n", ds.uploadKBps);
                    break;
                case SDLK_s:
                    ds.mbUploaded += 5.0f;
                    ds.uploadingMb = 2.0f;
                    printf("Upload step: +5MB (uploaded=%.1fMB)\n", ds.mbUploaded);
                    break;
                case SDLK_r:
                    ds = {};
                    strlcpy(ds.modemOp, "T-Mobile", sizeof(ds.modemOp));
                    printf("State reset\n");
                    break;
                default:
                    break;
                }
            }
        }

        // Decay USB write speed (simulates idle after write burst)
        if (ds.usbWriteKBps > 0.5f) ds.usbWriteKBps *= 0.95f;

        // Render display
        updateDisplay(ds);
        renderFramebuffer(renderer);

        SDL_Delay(100);  // ~10 fps
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

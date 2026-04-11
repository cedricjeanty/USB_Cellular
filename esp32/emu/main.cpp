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
//   T       Test AT command sequence via simulated UART
//   U       Simulate USB write (+10 MB display counter)
//   +/-     Adjust display upload speed
//   S       Step upload progress (+5 MB display counter)
//   R       Reset display state
//   Q/Esc   Quit

#define FW_VERSION "10.2001.7"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "hal/hal.h"
#include "hal/native_impls.h"
#include "hal/uart_pty.h"
#include "sim_modem.h"
#include "airbridge_display.h"
#include "airbridge_harvest.h"
#include "airbridge_s3.h"
#include "airbridge_modem.h"
#include "airbridge_triggers.h"
#include "airbridge_runtime.h"

// ── Display: same as TestDisplay but with SDL-compatible init ────────────────

class EmuDisplay : public IDisplay {
public:
    bool init() override { ok_ = true; clear(); return true; }
    void flush() override {}
    bool ok() const override { return ok_; }
private:
    bool ok_ = false;
};

// ── Clock: uses SDL ticks ───────────────────────────────────────────────────

class EmuClock : public IClock {
public:
    uint32_t millis() override { return SDL_GetTicks(); }
    void delay_ms(uint32_t ms) override { SDL_Delay(ms); }
};

// ── HAL instances (native implementations from shared headers) ──────────────

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
static SpeedTracker s_usbSpeed = {};
static SpeedTracker s_uploadSpeed = {};
static LogBuffer s_log;

// mdm_* function definitions for airbridge_modem.h (route through HAL UART)
int mdm_write(const void* data, size_t len) { return g_hal->uart->write(data, len); }
int mdm_read(void* buf, size_t len, uint32_t timeout_ms) { return g_hal->uart->read(buf, len, timeout_ms); }
void mdm_flush() { g_hal->uart->flush(); }
void mdm_set_baudrate(uint32_t baud) { g_hal->uart->set_baudrate(baud); }

int modem_at_cmd(const char* cmd, char* resp, int resp_size, int timeout_ms) {
    mdm_write(cmd, strlen(cmd));
    mdm_write("\r", 1);
    int total = 0;
    uint32_t start = g_hal->clock->millis();
    while ((g_hal->clock->millis() - start) < (uint32_t)timeout_ms && total < resp_size - 1) {
        uint8_t buf[128];
        int len = mdm_read(buf, sizeof(buf), 100);
        if (len > 0) {
            int copy = (len < resp_size - 1 - total) ? len : resp_size - 1 - total;
            memcpy(resp + total, buf, copy);
            total += copy;
            resp[total] = '\0';
            if (strstr(resp, "OK") || strstr(resp, "ERROR") || strstr(resp, "CONNECT")) break;
        }
    }
    resp[total] = '\0';
    return total;
}

// Background modem init thread — runs the same AT sequence as the real device
static std::atomic<bool> s_modemInitDone{false};
static ModemInitResult s_modemResult = {};

static void modemInitThread(DisplayState* ds) {
    printf("[Modem] Starting AT init sequence...\n");
    bool synced = modemAtSync();
    if (!synced) {
        printf("[Modem] AT sync failed\n");
        s_modemInitDone = true;
        return;
    }
    printf("[Modem] AT sync OK — running init...\n");
    s_modemResult = modemRunInit();
    printf("[Modem] Init complete: op=%s rssi=%d reg=%d ppp=%d\n",
           s_modemResult.operatorName, s_modemResult.rssi,
           s_modemResult.registered, s_modemResult.connected);

    // Update display state from modem result
    ds->modemReady = true;
    ds->modemRssi = s_modemResult.rssi;
    strlcpy(ds->modemOp, s_modemResult.operatorName, sizeof(ds->modemOp));
    if (s_modemResult.connected) {
        ds->pppConnected = true;
    }
    s_modemInitDone = true;
}

// ── S3 upload — uses shared uploadAllFiles() from airbridge_s3.h ────────────

static void doUpload(DisplayState& ds) {
    char harvestDir[256];
    snprintf(harvestDir, sizeof(harvestDir), "%s/harvested", SD_ROOT);
    printf("[S3] Uploading files from %s...\n", harvestDir);
    int n = uploadAllFiles(harvestDir);
    printf("[S3] %d file(s) uploaded\n", n);
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

    const char* deviceId = (argc > 1) ? argv[1] : "EMU000001";
    s_nvs.set_str("s3", "device_id", deviceId);

    char tmp[4] = "";
    if (!s_nvs.get_str("s3", "api_host", tmp, sizeof(tmp)) || tmp[0] == '\0') {
        s_nvs.set_str("s3", "api_host", "disw6oxjed.execute-api.us-west-2.amazonaws.com");
        s_nvs.set_str("s3", "api_key",  "7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm");
        printf("S3 credentials provisioned (first run)\n");
    }

    ::mkdir(SD_ROOT, 0755);

    s_modem = new SimModem();
    s_modem->operatorName = "SimOperator";
    s_modem->rssi = 22;
    s_modem->echoEnabled = false;
    if (s_modem->start()) {
        s_uart.fd = s_modem->slave_fd;
        printf("SIM7600 simulator running on PTY fd=%d\n", s_modem->slave_fd);
    }

    printf("AirBridge Emulator — device_id=%s\n", deviceId);
    printf("Virtual SD card: %s/\n", SD_ROOT);
    printf("NVS storage:     ./emu_nvs.dat\n\n");
    printf("Keys: C=cellular  H=harvest  P=upload  O=OTA-check  T=test-AT\n");
    printf("      I=status  U=usb-write  S=step  R=reset  Q=quit\n\n");
    s_log.clear();

    // Launch modem init in background (same AT sequence as real device)
    DisplayState ds = {};
    strlcpy(ds.modemOp, "Booting...", sizeof(ds.modemOp));
    std::thread modemThread(modemInitThread, &ds);

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
                case SDLK_t: {
                    if (s_uart.fd < 0) { printf("No modem simulator running\n"); break; }
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
                case SDLK_o:
                    if (!ds.pppConnected) { printf("Press C first\n"); break; }
                    printf("[OTA] Checking for update...\n");
                    { OtaCheckResult ota = halOtaCheck(FW_VERSION);
                      if (ota.status == 1) printf("[OTA] Update available: v%s (%u bytes)\n", ota.newVersion, ota.size);
                      else if (ota.status == 0) printf("[OTA] Up to date\n");
                      else printf("[OTA] Check failed\n");
                      s_log.write(g_hal->clock->millis(), "OTA check: status=%d", ota.status);
                    }
                    break;
                case SDLK_i: {
                    DeviceStatus st = {};
                    st.pppConnected = ds.pppConnected;
                    st.modemRssi = ds.modemRssi;
                    strlcpy(st.modemOp, ds.modemOp, sizeof(st.modemOp));
                    st.mbQueued = ds.mbQueued;
                    st.mbUploaded = ds.mbUploaded;
                    st.hostWrittenMb = ds.hostWrittenMb;
                    st.harvesting = false;
                    strlcpy(st.fwVersion, FW_VERSION, sizeof(st.fwVersion));
                    g_hal->nvs->get_str("s3", "api_host", st.apiHost, sizeof(st.apiHost));
                    g_hal->nvs->get_str("s3", "device_id", st.deviceId, sizeof(st.deviceId));
                    printf("--- STATUS ---\n%s--------------\n", formatStatus(st).c_str());
                    if (s_log.len > 0) printf("--- LOG ---\n%s-----------\n", s_log.contents().c_str());
                    break;
                }
                default:
                    break;
                }
            }
        }

        // Update speeds using shared SpeedTracker (same math as main_loop_task)
        uint32_t now = g_hal->clock->millis();
        ds.usbWriteKBps = s_usbSpeed.update(ds.hostWrittenMb, now);
        ds.uploadKBps = s_uploadSpeed.update(ds.mbUploaded + ds.uploadingMb, now);

        updateDisplay(ds);
        renderFramebuffer(renderer);
        SDL_Delay(100);
    }

    if (modemThread.joinable()) modemThread.join();
    if (s_modem) { s_modem->stop(); delete s_modem; }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

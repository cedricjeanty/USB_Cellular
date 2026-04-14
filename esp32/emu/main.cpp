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

#define FW_VERSION "20260412160000"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <sys/wait.h>
#include <signal.h>

#include "hal/hal.h"
#include "hal/native_impls.h"
#include "hal/uart_pty.h"
#include "sim_modem.h"
#include "airbridge_display.h"
#include "airbridge_harvest.h"
#include "airbridge_s3.h"
#include "airbridge_modem.h"
#include "sim_dsu.h"
#include "airbridge_triggers.h"
#include "airbridge_runtime.h"
#include "airbridge_log.h"

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

// ── Log PTY — external tools can `cat` it for real-time logs ────────────────
static int s_logPtyMaster = -1;
static char s_logPtyPath[64] = "";

static void _emu_serial_sink(const char* buf, int len) {
    // Write to stdout
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
    // Write to log PTY (if open)
    if (s_logPtyMaster >= 0) {
        ::write(s_logPtyMaster, buf, len);
    }
}

// Backward-compat: route cdc_printf through airbridge_log
static void cdc_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void cdc_printf(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n')) len--;
    buf[len] = '\0';
    if (len > 0) airbridge_log("%s", buf);
}
#define log_write airbridge_log

// Harvest pipeline state (mirrors firmware globals)
static bool     s_writeDetected = false;
static bool     s_hostWasConnected = false;
static bool     s_harvesting = false;
static uint32_t s_lastWriteMs = 0;
static uint32_t s_lastHarvestMs = 0;
static uint32_t s_harvestCoolMs = 30000;  // initial cooldown, then QUIET_WINDOW_MS

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

static pid_t s_clientPppd = -1;

// Simulated connection delay (real SIM7600 takes 20-30s for AT+registration+PPP)
#define MODEM_SIM_DELAY_MS 10000

static void modemInitThread(DisplayState* ds) {
    printf("[Modem] Starting AT init sequence...\n");

    // Phase 1: AT sync (2s simulated)
    usleep(2000000);
    bool synced = modemAtSync();
    if (!synced) {
        printf("[Modem] AT sync failed\n");
        s_modemInitDone = true;
        return;
    }
    printf("[Modem] AT sync OK — running init...\n");

    // Phase 2: Registration + operator (show "Connecting..." on display)
    ds->modemReady = true;
    usleep(3000000);  // 3s for registration

    s_modemResult = modemRunInit();
    printf("[Modem] Init complete: op=%s rssi=%d reg=%d ppp=%d\n",
           s_modemResult.operatorName, s_modemResult.rssi,
           s_modemResult.registered, s_modemResult.connected);

    ds->modemRssi = s_modemResult.rssi;
    strlcpy(ds->modemOp, s_modemResult.operatorName, sizeof(ds->modemOp));

    // Phase 3: PPP dial (5s simulated — real device takes 5-10s)
    usleep(5000000);

    if (s_modemResult.connected && s_modem) {
        // Start pppd on the slave fd — it negotiates PPP with SimModem
        // and creates a real ppp0 network interface.
        printf("[Modem] Starting pppd on %s...\n", s_modem->slavePath);

        // Close our slave fd so pppd can open it
        if (s_uart.fd >= 0) { close(s_uart.fd); s_uart.fd = -1; }

        s_clientPppd = fork();
        if (s_clientPppd == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) { dup2(devnull, 1); dup2(devnull, 2); close(devnull); }
            execlp("sudo", "sudo", "pppd",
                s_modem->slavePath,
                "921600",
                "noauth",
                "nodetach",
                "local",
                "nodefaultroute",
                "logfile", "/tmp/sim_pppd_client.log",
                (char*)nullptr);
            _exit(1);
        }

        if (s_clientPppd > 0) {
            // Wait for ppp interface to appear
            for (int i = 0; i < 30; i++) {
                usleep(500000);
                // Check for any ppp interface
                if (access("/sys/class/net/ppp0", F_OK) == 0 ||
                    access("/sys/class/net/ppp1", F_OK) == 0) {
                    ds->pppConnected = true;

                    // Policy routing: only emulator traffic goes through PPP
                    system("sudo ip rule del from 10.64.64.2 table 100 2>/dev/null");
                    system("sudo ip route flush table 100 2>/dev/null");
                    // Find which ppp interface has 10.64.64.2
                    char cmd[128];
                    snprintf(cmd, sizeof(cmd),
                        "PPP_IF=$(ip -o addr show | grep 10.64.64.2 | awk '{print $2}') && "
                        "sudo ip route add default dev $PPP_IF table 100 && "
                        "sudo ip rule add from 10.64.64.2 table 100 priority 100");
                    int rc = system(cmd);

                    (void)rc;
                    s_net.maxBytesPerSec = 100 * 1024;  // 100 KB/s matching real cellular

                    // Wait briefly for SimModem TUN to be ready (IPCP triggers openTun)
                    for (int t = 0; t < 20 && s_modem && !s_modem->tunReady(); t++)
                        usleep(100000);

                    if (s_modem && s_modem->tunReady()) {
                        // Route uploads through PPP → SimModem → TUN → internet
                        strlcpy(s_net.bindAddr, "10.64.64.2", sizeof(s_net.bindAddr));
                        printf("[Modem] PPP up — uploads route through SimModem TUN\n");
                    } else {
                        printf("[Modem] PPP up — TUN not available, uploads via direct internet\n");
                    }
                    s_log.write(0, "PPP tunnel up — all traffic via cellular sim");
                    break;
                }
            }
            if (!ds->pppConnected) {
                printf("[Modem] PPP interface didn't come up — falling back to direct\n");
                ds->pppConnected = true;
                s_net.maxBytesPerSec = 100 * 1024;  // 100 KB/s
            }
        }
    }
    s_modemInitDone = true;
}

// ── OTA progress — updates display during download ─────────────────────────

static void renderFramebuffer(SDL_Renderer* renderer);  // forward decl

static DisplayState* s_otaDs = nullptr;
static SDL_Renderer* s_otaRenderer = nullptr;

static void otaProgressCb(uint32_t sent, uint32_t total) {
    if (s_otaDs && total > 0) {
        s_otaDs->otaPct = (int)(sent * 100ULL / total);
        updateDisplay(*s_otaDs);
        if (s_otaRenderer) renderFramebuffer(s_otaRenderer);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {}
    }
}

// ── S3 upload — runs in background thread like real device ──────────────────

static std::atomic<bool> s_uploading{false};

static DisplayState* s_uploadDs = nullptr;

static void uploadProgressCb(uint32_t bytesSent, uint32_t totalBytes) {
    if (s_uploadDs) {
        s_uploadDs->uploadingMb = bytesSent / 1e6f;
    }
}

static void uploadThread(DisplayState* ds) {
    s_uploadDs = ds;
    char harvestDir[256];
    snprintf(harvestDir, sizeof(harvestDir), "%s/harvested", SD_ROOT);
    printf("[S3] Upload thread started — %s\n", harvestDir);

    char relPath[128];
    while (findNextUploadFile(harvestDir, relPath, sizeof(relPath))) {
        char fullpath[256];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", harvestDir, relPath);
        uint32_t sz = 0; bool isDir = false;
        g_hal->filesys->stat(fullpath, &sz, &isDir);
        float fileMb = sz / 1e6f;
        printf("[S3] Uploading %s (%.1f MB)...\n", relPath, fileMb);
        ds->uploadingMb = 0;

        UploadResult r = halS3UploadFile(fullpath, relPath, uploadProgressCb);
        ds->uploadingMb = 0;
        if (r.success) {
            printf("[S3] Upload complete: %.0f KB/s\n", r.kbps);
            markFileUploaded(harvestDir, relPath);
            ds->mbUploaded += fileMb;
            if (ds->mbQueued >= fileMb) ds->mbQueued -= fileMb; else ds->mbQueued = 0;
            s_log.write(g_hal->clock->millis(), "Uploaded %s %.0f KB/s", relPath, r.kbps);
        } else {
            printf("[S3] Upload failed: %s\n", r.error);
            s_log.write(g_hal->clock->millis(), "Upload FAIL %s: %s", relPath, r.error);
            break;
        }
    }
    s_uploadDs = nullptr;
    printf("[S3] Upload thread done\n");
    s_uploading = false;
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

    // Create log PTY for external tools (cat, test scripts)
    {
        int slaveFd = -1;
        char sname[64] = "";
        if (openpty(&s_logPtyMaster, &slaveFd, sname, nullptr, nullptr) == 0) {
            if (slaveFd >= 0) close(slaveFd);  // we only need the master side
            strlcpy(s_logPtyPath, sname, sizeof(s_logPtyPath));
            // Write path to file for E2E test discovery
            FILE* f = fopen("./emu_log.pty", "w");
            if (f) { fprintf(f, "%s\n", s_logPtyPath); fclose(f); }
        }
    }

    // Init unified logging with PTY+stdout sink
    airbridge_log_init(_emu_serial_sink, SDL_GetTicks);

    const char* deviceId = (argc > 1) ? argv[1] : "EMU000001";
    s_nvs.set_str("s3", "device_id", deviceId);

    airbridge_log("AirBridge fw=%s (emulator)", FW_VERSION);

    char tmp[4] = "";
    if (!s_nvs.get_str("s3", "api_host", tmp, sizeof(tmp)) || tmp[0] == '\0') {
        s_nvs.set_str("s3", "api_host", "disw6oxjed.execute-api.us-west-2.amazonaws.com");
        s_nvs.set_str("s3", "api_key",  "7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm");
        airbridge_log("S3 credentials provisioned (first run)");
    }

    ::mkdir(SD_ROOT, 0755);

    s_modem = new SimModem("./emu_modem.dat");
    s_modem->operatorName = "SimCellular";
    s_modem->rssi = 22;
    s_modem->echoEnabled = false;
    if (s_modem->start()) {
        s_uart.fd = s_modem->slave_fd;
        printf("SIM7600 simulator running on PTY fd=%d\n", s_modem->slave_fd);
    }

    printf("AirBridge Emulator — device_id=%s\n", deviceId);
    printf("Virtual SD card: %s/\n", SD_ROOT);
    printf("NVS storage:     ./emu_nvs.dat\n\n");
    printf("Auto: modem init → OTA check → file detect → harvest → upload\n");
    printf("Keys: D=DSU-session  I=status  T=test-AT  C=toggle-net  R=reset  Q=quit\n\n");
    fflush(stdout);
    setbuf(stdout, nullptr);  // disable buffering for real-time output
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

    // ── OTA check (like uploadTask does first after network) ────────
    // Wait for modem init in a non-blocking way
    static bool otaChecked = false;

    // Check for pending uploads from previous session (scan subfolders)
    {
        char harvestDir[256];
        snprintf(harvestDir, sizeof(harvestDir), "%s/harvested", SD_ROOT);
        char pendingRel[128];
        if (findNextUploadFile(harvestDir, pendingRel, sizeof(pendingRel))) {
            printf("[Boot] Found pending upload: %s — will upload after modem init\n", pendingRel);
            ds.mbQueued = 0;
            // Count pending files across all subfolders
            void* topDir = g_hal->filesys->opendir(harvestDir);
            if (topDir) {
                FsDirEntry subEnt;
                while (g_hal->filesys->readdir(topDir, &subEnt)) {
                    if (!subEnt.is_dir || subEnt.name[0] == '.') continue;
                    char subPath[256];
                    snprintf(subPath, sizeof(subPath), "%s/%s", harvestDir, subEnt.name);
                    void* subDir = g_hal->filesys->opendir(subPath);
                    if (!subDir) continue;
                    FsDirEntry ent;
                    while (g_hal->filesys->readdir(subDir, &ent)) {
                        if (ent.is_dir || ent.name[0] == '.') continue;
                        uint32_t sz = 0; bool isDir = false;
                        char fp[256]; snprintf(fp, sizeof(fp), "%s/%s", subPath, ent.name);
                        if (g_hal->filesys->stat(fp, &sz, &isDir) && !isDir)
                            ds.mbQueued += sz / 1e6f;
                    }
                    g_hal->filesys->closedir(subDir);
                }
                g_hal->filesys->closedir(topDir);
            }
        }
    }

    // Boot splash (same as firmware app_main — holds for 5s)
    bool running = true;
    {
        char devId[16] = "";
        s_nvs.get_str("s3", "device_id", devId, sizeof(devId));
        dispBootSplash(FW_VERSION, devId[0] ? devId : deviceId);
        renderFramebuffer(renderer);
        uint32_t splashEnd = SDL_GetTicks() + 5000;
        while (SDL_GetTicks() < splashEnd && running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) { if (ev.type == SDL_QUIT) running = false; }
            SDL_Delay(100);
        }
    }
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
                    // Keep RSSI on disconnect — matches firmware fix (don't zero signal)
                    printf("Cellular: %s\n", ds.pppConnected ? "ON (using laptop internet)" : "OFF");
                    break;
                case SDLK_h: {
                    printf("Harvesting from %s/ ...\n", SD_ROOT);
                    char destDir[256];
                    snprintf(destDir, sizeof(destDir), "%s/harvested", SD_ROOT);
                    uint32_t hnum = 0;
                    g_hal->nvs->get_u32("harvest", "count", &hnum);
                    hnum++;
                    g_hal->nvs->set_u32("harvest", "count", hnum);
                    HarvestResult r = harvestFiles(SD_ROOT, destDir, (uint16_t)hnum);
                    printf("Harvested: %u file(s), %.1f MB → %s\n", r.count, r.usedMb, r.folder);
                    ds.mbQueued += r.usedMb;
                    break;
                }
                case SDLK_p:
                    if (!ds.pppConnected) {
                        printf("Press C first to enable network\n");
                    } else if (s_uploading) {
                        printf("Upload already in progress\n");
                    } else {
                        s_uploading = true;
                        std::thread(uploadThread, &ds).detach();
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
                case SDLK_d: {
                    printf("[DSU] Running download session...\n");
                    SimDSU dsu;
                    dsu.sdRoot = SD_ROOT;
                    SimDSU::SessionResult sr = dsu.runSession(4.0f);  // 4MB
                    if (sr.success) {
                        printf("[DSU] Session complete: %s (%u bytes)\n", sr.filename, sr.bytesWritten);
                    } else {
                        printf("[DSU] Session failed\n");
                    }
                    break;
                }
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

        // ── Automated pipeline (mirrors main_loop_task) ─────────────
        uint32_t now = g_hal->clock->millis();

        // OTA check — show "Checking..." first, then run OTA on next frame
        static bool otaReadyToRun = false;
        if (!otaChecked && !ds.otaActive) {
            ds.otaActive = true;
            ds.otaPct = -1;
        }

        if (!otaChecked && s_modemInitDone && ds.pppConnected && !otaReadyToRun) {
            // Modem just connected — render one frame with "Checking..." before blocking
            otaReadyToRun = true;
            updateDisplay(ds);
            renderFramebuffer(renderer);
            continue;  // let the frame display, run OTA on next iteration
        }

        if (!otaChecked && otaReadyToRun) {
            otaChecked = true;
            printf("[OTA] Checking for firmware update (current=%s)...\n", FW_VERSION);

            OtaCheckResult ota = halOtaCheck(FW_VERSION);
            if (ota.status == 1) {
                printf("[OTA] Update available: v%s (%u bytes)\n", ota.newVersion, ota.size);
                strlcpy(ds.otaVersion, ota.newVersion, sizeof(ds.otaVersion));
                ds.otaPct = 0;
                updateDisplay(ds); renderFramebuffer(renderer);

                printf("[OTA] Downloading...\n");
                s_otaDs = &ds;
                s_otaRenderer = renderer;
                OtaDownloadResult dl = halOtaDownload(ota, "./emu_ota_update.bin",
                    otaProgressCb);

                if (dl.success) {
                    printf("[OTA] Downloaded %u bytes → emu_ota_update.bin\n", dl.bytesDownloaded);
                    ds.otaPct = 100;
                    g_hal->nvs->set_str("ota", "pending_version", ota.newVersion);
                    s_log.write(now, "OTA: downloaded v%s (%u bytes)", ota.newVersion, dl.bytesDownloaded);
                    updateDisplay(ds); renderFramebuffer(renderer);
                    SDL_Delay(2000);
                } else {
                    printf("[OTA] Download failed: %s\n", dl.error);
                    s_log.write(now, "OTA: download failed: %s", dl.error);
                }
            } else if (ota.status == 0) {
                printf("[OTA] Up to date\n");
                s_log.write(now, "OTA: up to date");
            } else {
                printf("[OTA] Check failed (network error)\n");
                s_log.write(now, "OTA: check failed");
            }
            ds.otaActive = false;

            // Fetch S3 DSU cookie (same as firmware uploadTask)
            {
                char apiHost[128] = "", apiKey[64] = "";
                g_hal->nvs->get_str("s3", "api_host", apiHost, sizeof(apiHost));
                g_hal->nvs->get_str("s3", "api_key", apiKey, sizeof(apiKey));
                if (apiHost[0] && apiKey[0]) {
                    TlsHandle tls = g_hal->network->connect(apiHost);
                    if (tls) {
                        char req[512];
                        snprintf(req, sizeof(req),
                            "GET /prod/firmware/cookie HTTP/1.1\r\nHost: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
                            apiHost, apiKey);
                        g_hal->network->write(tls, req, strlen(req));
                        std::string resp = halHttpReadResponse(tls);
                        g_hal->network->destroy(tls);

                        std::string hexStr = jsonStr(resp, "cookie");
                        if (hexStr.size() == 156) {
                            uint8_t cookie[78];
                            for (int i = 0; i < 78; i++) {
                                char h[3] = { hexStr[i*2], hexStr[i*2+1], 0 };
                                cookie[i] = (uint8_t)strtol(h, nullptr, 16);
                            }
                            if (cookie[0] == 0xEA && cookie[1] == 0x1E) {
                                char cp[256];
                                snprintf(cp, sizeof(cp), "%s/dsuCookie.easdf", SD_ROOT);
                                void* cf = g_hal->filesys->open(cp, "wb");
                                if (cf) {
                                    g_hal->filesys->write(cf, cookie, 78);
                                    g_hal->filesys->close(cf);
                                    printf("[S3] Cookie applied to SD\n");
                                }
                            }
                        } else {
                            printf("[S3] No cookie available\n");
                        }
                    }
                }
            }
        }

        // Auto-upload pending files after modem connects
        static bool bootUploadChecked = false;
        if (!bootUploadChecked && s_modemInitDone && ds.pppConnected && ds.mbQueued > 0.0f && !s_uploading) {
            bootUploadChecked = true;
            printf("[Boot] Starting upload of pending files (%.1f MB)\n", ds.mbQueued);
            s_uploading = true;
            std::thread(uploadThread, &ds).detach();
        }

        // Poll for new files in emu_sdcard/ + subdirs (simulates USB MSC writes)
        {
            static uint32_t lastScanMs = 0;
            static int lastFileCount = -1;
            if (now - lastScanMs > 2000) {  // scan every 2s
                lastScanMs = now;
                int count = 0;
                // Count files in root + one level of subdirectories (like flightHistory/)
                auto countDir = [&](const char* path) {
                    void* dir = g_hal->filesys->opendir(path);
                    if (!dir) return;
                    FsDirEntry ent;
                    while (g_hal->filesys->readdir(dir, &ent)) {
                        if (ent.name[0] == '.') continue;
                        if (strcmp(ent.name, "harvested") == 0) continue;
                        if (isSkipped(ent.name)) continue;
                        char fp[256]; snprintf(fp, sizeof(fp), "%s/%s", path, ent.name);
                        uint32_t sz = 0; bool isDir = false;
                        g_hal->filesys->stat(fp, &sz, &isDir);
                        if (isDir) {
                            // Recurse one level
                            void* sub = g_hal->filesys->opendir(fp);
                            if (sub) {
                                FsDirEntry se;
                                while (g_hal->filesys->readdir(sub, &se)) {
                                    if (se.name[0] == '.') continue;
                                    char sfp[384]; snprintf(sfp, sizeof(sfp), "%s/%s", fp, se.name);
                                    uint32_t ssz = 0; bool sd = false;
                                    if (g_hal->filesys->stat(sfp, &ssz, &sd) && !sd && ssz > 0)
                                        count++;
                                }
                                g_hal->filesys->closedir(sub);
                            }
                        } else if (sz > 0) {
                            count++;
                        }
                    }
                    g_hal->filesys->closedir(dir);
                };
                countDir(SD_ROOT);
                if (lastFileCount >= 0 && count > lastFileCount) {
                    // New files detected — simulate USB write
                    s_writeDetected = true;
                    s_lastWriteMs = now;
                    s_hostWasConnected = true;
                    float newMb = 0;
                    // Estimate total size
                    void* d2 = g_hal->filesys->opendir(SD_ROOT);
                    if (d2) {
                        FsDirEntry e2;
                        while (g_hal->filesys->readdir(d2, &e2)) {
                            if (e2.name[0] == '.' || strcmp(e2.name, "harvested") == 0) continue;
                            uint32_t sz = 0; bool isDir = false;
                            char fp[256]; snprintf(fp, sizeof(fp), "%s/%s", SD_ROOT, e2.name);
                            if (g_hal->filesys->stat(fp, &sz, &isDir) && !isDir)
                                newMb += sz / 1e6f;
                        }
                        g_hal->filesys->closedir(d2);
                    }
                    ds.hostWrittenMb = newMb;
                    printf("[USB] New files detected (%d → %d), %.1f MB\n",
                           lastFileCount, count, newMb);
                }
                lastFileCount = count;
            }
        }

        // Harvest trigger — uses shared shouldHarvest() (same logic as firmware)
        if (shouldHarvest(s_harvesting, s_writeDetected, s_hostWasConnected,
                          s_lastWriteMs, s_lastHarvestMs, s_harvestCoolMs, now)) {
            printf("[Harvest] Triggering (%.1f MB written, %us idle)\n",
                   ds.hostWrittenMb, (now - s_lastWriteMs) / 1000);
            s_harvesting = true;

            char destDir[256];
            snprintf(destDir, sizeof(destDir), "%s/harvested", SD_ROOT);
            uint32_t hnum = 0;
            g_hal->nvs->get_u32("harvest", "count", &hnum);
            hnum++;
            g_hal->nvs->set_u32("harvest", "count", hnum);
            HarvestResult r = harvestFiles(SD_ROOT, destDir, (uint16_t)hnum);
            printf("[Harvest] Done: %u file(s), %.1f MB → %s\n", r.count, r.usedMb, r.folder);
            ds.mbQueued += r.usedMb;

            // Write DSU cookie if flight history files were harvested
            if (r.maxFlight > 0 && r.dsuSerial[0]) {
                uint8_t cookie[78];
                buildDsuCookie(r.dsuSerial, r.maxFlight, cookie);
                char cookiePath[256];
                snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", SD_ROOT);
                void* cf = g_hal->filesys->open(cookiePath, "wb");
                if (cf) {
                    g_hal->filesys->write(cf, cookie, 78);
                    g_hal->filesys->close(cf);
                    printf("[Harvest] Cookie: %s flight %u\n", r.dsuSerial, r.maxFlight);
                }
            }

            s_writeDetected = false;
            s_lastWriteMs = 0;
            s_hostWasConnected = false;
            s_lastHarvestMs = now;
            s_harvestCoolMs = QUIET_WINDOW_MS;
            s_harvesting = false;

            // Auto-upload if connected (new harvest or pending from previous session)
            if (ds.pppConnected && (r.count > 0 || ds.mbQueued > 0.001f) && !s_uploading) {
                s_uploading = true;
                std::thread(uploadThread, &ds).detach();
            }
        }

        // Update speeds using shared SpeedTracker (same math as main_loop_task)
        ds.usbWriteKBps = s_usbSpeed.update(ds.hostWrittenMb, now);
        ds.uploadKBps = s_uploadSpeed.update(ds.mbUploaded + ds.uploadingMb, now);

        updateDisplay(ds);
        renderFramebuffer(renderer);
        SDL_Delay(100);
    }

    if (modemThread.joinable()) modemThread.join();
    // Clean up pppd and routing
    if (s_clientPppd > 0) {
        system("sudo ip rule del from 10.64.64.2 table 100 2>/dev/null");
        system("sudo ip route flush table 100 2>/dev/null");
        kill(s_clientPppd, SIGTERM);
        waitpid(s_clientPppd, nullptr, 0);
    }
    if (s_modem) { s_modem->stop(); delete s_modem; }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

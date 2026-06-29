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
//   D       Run DSU download session (EMU_DSU_FILE or test/fixtures/EA500.000243_01077.eaofh)
//   H       Harvest files from emu_sdcard/ to emu_sdcard/upload/
//   P       Upload files from emu_sdcard/upload/ to S3 (requires C first)
//   T       Test AT command sequence via simulated UART
//   U       Simulate USB write (+10 MB display counter)
//   +/-     Adjust display upload speed
//   S       Step upload progress (+5 MB display counter)
//   R       Reset display state
//   B       Break (corrupt) the SD FAT — only with EMU_SD_BLOCK=1; drives the
//           degrade→reformat→recover demo (real FatFs on an in-memory FakeSd)
//   Q/Esc   Quit
//
// EMU_SD_BLOCK=1 backs the SD with a real FAT image on an in-memory FakeSd block
// device (vs the default host dirs), so corruption/reformat is genuine. Headless:
// EMU_SD_CORRUPT_AFTER_MS=N auto-corrupts; SIGUSR2 corrupts on demand. See
// scripts/test_emu_sd_block.sh.

#define FW_VERSION "20260412160000"

#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>   // stat() for the EMU_CELL_FILE mtime poll
#include <dirent.h>     // opendir/readdir for the DSU transfer cursor
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/wait.h>
#include <signal.h>

#include "hal/hal.h"
#include "hal/native_impls.h"
#include "hal/uart_pty.h"
#include "hal/fatfs_filesys.h"   // EMU_SD_BLOCK=1: SD via real FatFs on FakeSd
#include "sim_modem.h"
#include "airbridge_display.h"
#include "airbridge_harvest.h"
#include "airbridge_s3.h"
#include "airbridge_commands.h"   // runCommandTextBuffer + shared g_compress (remote cmd channel)
#include "airbridge_modem.h"
#include "sim_dsu.h"
#include "airbridge_triggers.h"
#include "airbridge_runtime.h"
#include "airbridge_log.h"
#include <csignal>

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
// Set by SIGUSR1 — main loop checks and toggles cellular (mirrors 'C' keypress)
static volatile sig_atomic_t s_cellularToggle = 0;
static void sigusr1_handler(int) { s_cellularToggle = 1; }
// EMU_CELL_FILE: a control file the test harness writes (atomically) to script a
// time-varying cellular profile — "full" | "weak" | "off" | "flaky [pct]". Polled
// in the main loop (mtime-gated). Empty/unset ⇒ boot default stays "full", so
// existing tests are unaffected. See applyCellState() + scripts/flight_cycle_test.sh.
static std::string s_cellFilePath;
static time_t      s_cellFileMtime = 0;

// EMU_DSU_INTERNAL: path to a synthetic "aircraft internal memory" .eaofh holding
// the DSU's backlog. When set, a transfer thread streams un-sent flights into
// flightHistory/ rate-limited (SimDSU's ~500 KB/s USB rate) starting
// EMU_DSU_DELAY_MS after boot (the 90s USB-present moment). Models a NEW aircraft
// whose full log backlog lives in the DSU and must transfer over USB before the
// device can upload it. See dsuTransferThread() + scripts/flight_cycle_test.sh.
static std::atomic<bool> s_dsuStop{false};

// EMU_COMPRESS=1: gzip .eaofh files into the upload queue at harvest (~3x fewer
// bytes on the cellular PUT). Mirrors the firmware's opt-in compression.
static bool s_compress = false;

// Modeled on-device gzip throughput (bytes/sec). The emulator gzips on the host CPU
// (effectively instant), which would OVERSTATE compression's benefit vs hardware where
// the deflate runs ~178 KB/s (measured, level 1 — see CZ_DEVICE_GZIP_BPS). So after a
// compressing harvest we sleep inBytes/rate to charge the device's serial gzip-before-
// upload CPU time, making the catch-up A/B reflect reality. EMU_GZIP_KBPS overrides.
#ifndef CZ_DEVICE_GZIP_BPS
#define CZ_DEVICE_GZIP_BPS 178000
#endif
static uint32_t s_gzipBps = CZ_DEVICE_GZIP_BPS;

// Charge the modeled on-device gzip CPU time for `inBytes` of uncompressed input.
static void modelGzipTime(uint64_t inBytes) {
    if (!s_compress || s_gzipBps == 0 || inBytes == 0) return;
    uint32_t ms = (uint32_t)((inBytes * 1000ULL) / s_gzipBps);
    printf("[Harvest] modeled on-device gzip: %llu bytes / %u Bps = %u ms\n",
           (unsigned long long)inBytes, s_gzipBps, ms);
    SDL_Delay(ms);
}

// Emulator uplink throttle (bytes/sec) for the upload path. Default 100 KB/s
// (historical). EMU_UPLINK_KBPS overrides — e.g. 167 to match the real PCB's
// sustained 3 Mbaud + HW-FC cellular rate, so soak-test cycle counts reflect
// real throughput.
static int emuUplinkBytesPerSec() {
    const char* e = getenv("EMU_UPLINK_KBPS");
    int kbps = (e && atoi(e) > 0) ? atoi(e) : 100;
    return kbps * 1024;
}
static const char* SD_ROOT = "./emu_sdcard";           // Partition 1 (DSU-facing)
static const char* SD_INTERNAL = "./emu_sdcard_internal"; // Partition 2 (firmware internal)
// EMU_SD_BLOCK=1: route the SD through real FatFs on an in-memory FakeSd block
// device (vs the default host-directory backend), so FAT corruption can be
// injected ('B' keypress / SIGUSR2 / EMU_SD_CORRUPT_AFTER_MS) and the firmware's
// degrade→reformat recovery exercised end-to-end with the OLED showing SD ERROR.
static FatFsFilesys* s_blockFs = nullptr;
static bool s_sdBlockMode = false;
static volatile sig_atomic_t s_sdCorrupt = 0;
static void sigusr2_handler(int) { s_sdCorrupt = 1; }
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

// Signal handler for clean shutdown on SIGTERM/SIGINT
static volatile bool s_quit = false;
static void sigHandler(int) { s_quit = true; }

// ── Simulated USB MSC SD-bus contender ──────────────────────────────────────
// Models the host hammering the SD card via USB MSC raw-sector reads while the
// firmware uploads: once "USB is presented", repeatedly grab the shared SD bus
// with a 100ms timeout (exactly like tud_msc_read10_cb on the device), hold it
// for a simulated sector read, release. Reproduces the MSC↔upload SD contention
// the real device hits — which the emulator otherwise never models. Enabled via
// EMU_SD_CONTENTION=1 (sets readKBps + spawns this thread).
static std::atomic<bool> s_mscActive{false};   // true once "USB presented"
static void mscContenderThread(SimSdCard* sd) {
    while (!s_quit) {
        if (!s_mscActive.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        // Host MSC read10: take the SD bus with a 100ms timeout (else returns -1 to host)
        if (sd->bus.try_lock_for(std::chrono::milliseconds(100))) {
            sd->mscReads++;
            std::this_thread::sleep_for(std::chrono::milliseconds(3));  // ~one MSC sector burst over SPI
            sd->bus.unlock();
        } else {
            sd->mscTimeouts++;  // host read timed out — on HW the host then retries / may reset
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));  // host read cadence
    }
}

// Harvest pipeline state (mirrors firmware globals)
static bool     s_writeDetected = false;
static bool     s_hostWasConnected = false;
static bool     s_harvesting = false;
static uint32_t s_lastWriteMs = 0;
static bool     s_bootHarvestPending = false;
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
                    s_net.maxBytesPerSec = emuUplinkBytesPerSec();  // EMU_UPLINK_KBPS or 100 KB/s default

                    // Wait briefly for SimModem TUN to be ready (IPCP triggers openTun)
                    for (int t = 0; t < 20 && s_modem && !s_modem->tunReady(); t++)
                        usleep(100000);

                    if (s_modem && s_modem->tunReady()) {
                        // Route uploads through PPP → SimModem → TUN → internet
                        strlcpy(s_net.bindAddr, "10.64.64.2", sizeof(s_net.bindAddr));
                        printf("[Modem] PPP up — uploads route through SimModem TUN\n");
                        if (getenv("EMU_PROF")) printf("[PROF] ppp_up t=%ums\n", SDL_GetTicks());
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
                s_net.maxBytesPerSec = emuUplinkBytesPerSec();
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
        if (s_otaRenderer) {
            renderFramebuffer(s_otaRenderer);
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {}
        }
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
    snprintf(harvestDir, sizeof(harvestDir), "%s/upload", SD_INTERNAL);
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

        // Route .eaofh files through fleet-aware upload (manifest check + aircraft path)
        const char* fname = strrchr(relPath, '/');
        fname = fname ? fname + 1 : relPath;
        const char* bareName = fname;
        for (const char* p = fname; p[0] && p[1]; p++)
            if (p[0] == '_' && p[1] == '_') bareName = p + 2;
        const char* ext = strrchr(bareName, '.');
        bool isEaofh = ext && strcmp(ext, ".eaofh") == 0;

        UploadResult r = isEaofh
            ? halS3UploadEaofh(harvestDir, relPath, uploadProgressCb)
            : halS3UploadFile(fullpath, relPath, uploadProgressCb);
        ds->uploadingMb = 0;
        if (r.success) {
            printf("[S3] Upload complete: %.0f KB/s (%s)\n", r.kbps, r.error);
            markFileUploaded(harvestDir, relPath);
            // Also clean up .meta sidecar if present (halS3UploadEaofh does it too, belt+suspenders)
            char metaPath[272];
            snprintf(metaPath, sizeof(metaPath), "%s.meta", fullpath);
            g_hal->filesys->remove(metaPath);
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
    if (!renderer) return;  // headless: nothing to draw
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

// Apply a cellular-profile state word (from EMU_CELL_FILE) to the emulated link.
// Maps the word → ds.pppConnected (gates uploads) + sim_modem signal members +
// flaky %. "off" drops the real PPP session on the on→off transition (cutting any
// in-flight upload, like losing signal at takeoff); the firmware then reconnects
// when a connected state ("full"/"weak"/"flaky") returns. Returns true if the word
// was recognized (state changed).
static bool applyCellState(const char* word, DisplayState& ds, SimModem* m) {
    char w[16] = ""; int pct = 0;
    sscanf(word, "%15s %d", w, &pct);              // e.g. "flaky 30" → w="flaky", pct=30
    bool wasOn = ds.pppConnected;
    if (!strcmp(w, "full")) {
        ds.pppConnected = true; ds.modemReady = true; ds.modemRssi = 22;
        if (m) { m->rssi = 22; m->rsrpIdx = 70; m->rsrqIdx = 26; m->sinr = 12; m->setFlaky(0); }
    } else if (!strcmp(w, "weak")) {
        ds.pppConnected = true; ds.modemReady = true; ds.modemRssi = 8;
        if (m) { m->rssi = 8; m->rsrpIdx = 30; m->rsrqIdx = 10; m->sinr = 2; m->setFlaky(0); }
    } else if (!strcmp(w, "off")) {
        if (m) { m->setFlaky(0); if (wasOn && m->pppUp) m->dropSession(); }  // cut link only on on→off
        ds.pppConnected = false;
    } else if (!strcmp(w, "flaky")) {
        if (pct <= 0) pct = 30;
        ds.pppConnected = true; ds.modemReady = true; ds.modemRssi = 12;
        if (m) { m->rssi = 12; m->setFlaky(pct); }
    } else {
        return false;                               // unknown word — ignore
    }
    printf("[CELL] state=%s rssi=%d flaky=%d ppp=%d\n", w, ds.modemRssi,
           m ? m->flakyDropPct.load() : 0, ds.pppConnected ? 1 : 0);
    airbridge_log("CELL: profile=%s ppp=%d flaky=%d", w, ds.pppConnected ? 1 : 0,
                  m ? m->flakyDropPct.load() : 0);
    return true;
}

// Highest flight number already present as a file in flightHistory/. Filenames
// are "<serial>_<NNNNN>_<date>.eaofh"; the serial has no '_', so the first '_'
// is followed by the flight number. Used (with the cookie) as the DSU transfer
// resume point so flights already on the device aren't re-sent over USB.
static uint32_t highestFlightInFlightHistory(const char* root) {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/flightHistory", root);
    DIR* d = opendir(dir);
    if (!d) return 0;
    uint32_t hi = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;        // skip ".", "..", ".*.part"
        const char* us = strchr(e->d_name, '_');
        if (!us) continue;
        uint32_t f = (uint32_t)atoi(us + 1);
        if (f > hi) hi = f;
    }
    closedir(d);
    return hi;
}

// DSU transfer thread: models the aircraft DSU connecting over USB ~90s after
// power-on and streaming its un-sent flight backlog onto the device, rate-limited
// at SimDSU's writeSpeedKBps (~500 KB/s real USB rate). One file per flight,
// atomically (a kill at power-off leaves the already-transferred flights intact —
// incremental progress that the next boot resumes via the cookie). Runs once per
// emulator process (one cycle); the test appends the just-flown flight to the
// internal memory after the cycle so it transfers on the *next* power-on.
static void dsuTransferThread(std::string internalPath, uint32_t delayMs, std::string sdRoot) {
    // Wait out the USB-present delay (scaled by the test for --fast), but stay
    // responsive to an early power-off.
    uint32_t waited = 0;
    while (waited < delayMs && !s_dsuStop) {
        uint32_t step = (delayMs - waited > 50) ? 50 : (delayMs - waited);
        usleep(step * 1000);
        waited += step;
    }
    if (s_dsuStop) return;

    SimDSU dsu;
    dsu.sdRoot = sdRoot.c_str();
    dsu.internalMemoryPath = internalPath.c_str();
    if (const char* m = getenv("EMU_DSU_KBPS"); m && atoi(m) > 0) dsu.writeSpeedKBps = atoi(m);

    uint32_t cookie = dsu.readCookie();
    uint32_t inFh   = highestFlightInFlightHistory(sdRoot.c_str());
    uint32_t resume = cookie > inFh ? cookie : inFh;

    const auto& idx = dsu.flightIndex();   // builds once
    printf("[DSU] Transfer thread: %zu flight(s) in memory, resume after %u "
           "(cookie=%u fh=%u) @%dKB/s\n",
           idx.size(), resume, cookie, inFh, dsu.writeSpeedKBps);

    uint32_t sent = 0;
    for (const auto& e : idx) {
        if (s_dsuStop) break;
        if (e.flight <= resume) continue;
        SimDSU::SessionResult r = dsu.transferFlight(e.flight);
        if (r.success) {
            sent++;
            printf("[DSU] USB→device: flight %u (%u bytes)\n", e.flight, r.bytesWritten);
        }
    }
    printf("[DSU] Transfer thread done: %u flight(s) this cycle%s\n",
           sent, s_dsuStop ? " (interrupted by power-off)" : "");
}

// ── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    signal(SIGTERM, sigHandler);
    signal(SIGINT, sigHandler);
    signal(SIGUSR1, sigusr1_handler);  // test suite sends SIGUSR1 to toggle cellular
    signal(SIGUSR2, sigusr2_handler);  // test suite sends SIGUSR2 to corrupt the SD (block mode)
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

    // Parse args: first non-flag arg is the device id. The OLED window is opt-in
    // (--render / -r, or EMU_RENDER=1) — integration tests run headless by default.
    const char* deviceId = "EMU000001";
    bool g_render = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--render") == 0 || strcmp(argv[i], "-r") == 0) g_render = true;
        else if (argv[i][0] != '-') deviceId = argv[i];
    }
    if (const char* e = getenv("EMU_RENDER"); e && (e[0] == '1' || e[0] == 'y' || e[0] == 'Y'))
        g_render = true;

    // ── SD/MSC contention model (EMU_SD_CONTENTION=1) ────────────────────────
    // Installs the shared SD bus + a simulated USB-MSC reader that competes for
    // it, plus per-read SPI latency (EMU_SD_KBPS, default 250 KB/s ≈ observed
    // SPI throughput). Reproduces the device's concurrent SD access; off by
    // default so normal runs/tests are unaffected.
    static SimSdCard s_simSd;
    std::thread mscThread;
    if (const char* e = getenv("EMU_SD_CONTENTION"); e && e[0] == '1') {
        const char* kb = getenv("EMU_SD_KBPS");
        s_simSd.readKBps = kb ? atoi(kb) : 250;
        g_simSdCard() = &s_simSd;
        s_mscActive = true;  // host MSC is hammering the card (conservative: whole run)
        mscThread = std::thread(mscContenderThread, &s_simSd);
        printf("SD contention model: ON (SPI readKBps=%d, MSC contender active)\n", s_simSd.readKBps);
    }

    // ── Flaky-link fault injection (EMU_DROP_PUT_RESP=N) ─────────────────────
    // The Nth S3 part PUT uploads its body but its response is dropped (lost ack),
    // so the firmware retries the part. Exercises the multipart part-PUT retry.
    if (const char* e = getenv("EMU_DROP_PUT_RESP"); e && atoi(e) > 0) {
        s_net.dropPutResponseOnPart = atoi(e);
        printf("Flaky-link: drop response of part PUT #%d (retry test)\n", s_net.dropPutResponseOnPart);
    }
    // EMU_CELL_FILE: scripted cellular-profile control file (full|weak|off|flaky).
    if (const char* e = getenv("EMU_CELL_FILE"); e && e[0]) {
        s_cellFilePath = e;
        printf("Cellular profile: scripted via %s\n", s_cellFilePath.c_str());
    }
    if (const char* e = getenv("EMU_CELL_SEED"); e && e[0]) srand((unsigned)atoi(e));
    if (const char* e = getenv("EMU_COMPRESS"); e && e[0] == '1') {
        s_compress = true;
        printf("Harvest compression: ON (gzip .eaofh into upload queue)\n");
    }
    if (const char* e = getenv("EMU_GZIP_KBPS"); e && atoi(e) > 0) {
        s_gzipBps = (uint32_t)atoi(e) * 1000u;
        printf("Modeled on-device gzip rate: %u KB/s\n", (unsigned)(s_gzipBps / 1000u));
    }

    // EMU_DSU_INTERNAL: backlog-in-the-DSU transfer thread (see dsuTransferThread).
    std::thread dsuThread;
    if (const char* e = getenv("EMU_DSU_INTERNAL"); e && e[0]) {
        uint32_t delayMs = 90000;  // default: real 90s USB-present delay
        if (const char* d = getenv("EMU_DSU_DELAY_MS"); d && d[0]) delayMs = (uint32_t)atol(d);
        printf("DSU backlog: %s (USB-present transfer at +%ums)\n", e, delayMs);
        dsuThread = std::thread(dsuTransferThread, std::string(e), delayMs, std::string(SD_ROOT));
    }

    s_nvs.set_str("s3", "device_id", deviceId);

    // Session counter (monotonic) + session log file (same as firmware)
    uint32_t emuBootCount = 0;
    s_nvs.get_u32("dbg", "session", &emuBootCount);
    emuBootCount++;
    s_nvs.set_u32("dbg", "session", emuBootCount);
    char emuLogSession[32];
    snprintf(emuLogSession, sizeof(emuLogSession), "boot_%04lu", (unsigned long)emuBootCount);
    char emuLogPath[256];
    snprintf(emuLogPath, sizeof(emuLogPath), "%s/logs", SD_INTERNAL);
    ::mkdir(emuLogPath, 0755);
    snprintf(emuLogPath, sizeof(emuLogPath), "%s/logs/%s.log", SD_INTERNAL, emuLogSession);

    airbridge_log("AirBridge fw=%s boot=%lu (emulator)", FW_VERSION, (unsigned long)emuBootCount);

    // Move old boot logs directly into upload queue on P2 (internal partition).
    // With dual-partition, harvest scans P1 (DSU) — old logs on P2 won't be found
    // by harvest, so we put them straight into upload/NNNN/ for the upload pipeline.
    {
        char logDir[256];
        snprintf(logDir, sizeof(logDir), "%s/logs", SD_INTERNAL);
        void* dir = g_hal->filesys->opendir(logDir);
        if (dir) {
            FsDirEntry ent;
            std::vector<std::string> oldLogs;
            while (g_hal->filesys->readdir(dir, &ent)) {
                if (ent.is_dir) continue;
                if (strncmp(ent.name, "boot_", 5) != 0) continue;
                const char* dot = strrchr(ent.name, '.');
                if (!dot || strcmp(dot, ".log") != 0) continue;
                char session[48];
                size_t nameLen = dot - ent.name;
                if (nameLen >= sizeof(session)) continue;
                memcpy(session, ent.name, nameLen);
                session[nameLen] = '\0';
                if (strcmp(session, emuLogSession) == 0) continue;
                oldLogs.push_back(ent.name);
            }
            g_hal->filesys->closedir(dir);
            if (!oldLogs.empty()) {
                uint32_t hnum = 0;
                g_hal->nvs->get_u32("harvest", "count", &hnum);
                hnum++;
                g_hal->nvs->set_u32("harvest", "count", hnum);
                char destDir[256];
                snprintf(destDir, sizeof(destDir), "%s/upload/%04u", SD_INTERNAL, hnum);
                ::mkdir(destDir, 0755);
                char uploadBase[256];
                snprintf(uploadBase, sizeof(uploadBase), "%s/upload", SD_INTERNAL);
                ::mkdir(uploadBase, 0755);
                ::mkdir(destDir, 0755);
                for (auto& name : oldLogs) {
                    char src[256], dst[256];
                    snprintf(src, sizeof(src), "%s/logs/%s", SD_INTERNAL, name.c_str());
                    snprintf(dst, sizeof(dst), "%s/%s", destDir, name.c_str());
                    if (rename(src, dst) == 0) {
                        airbridge_log("Moved old log %s to upload queue", name.c_str());
                    }
                }
            }
        }
    }

    char tmp[4] = "";
    if (!s_nvs.get_str("s3", "api_host", tmp, sizeof(tmp)) || tmp[0] == '\0') {
        s_nvs.set_str("s3", "api_host", "disw6oxjed.execute-api.us-west-2.amazonaws.com");
        s_nvs.set_str("s3", "api_key",  "7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm");
        airbridge_log("S3 credentials provisioned (first run)");
    }

    ::mkdir(SD_ROOT, 0755);
    ::mkdir(SD_INTERNAL, 0755);

    // ── Optional: SD via real FatFs on an in-memory FakeSd block device ──────
    // The default backend is host directories (NativeFilesys). With EMU_SD_BLOCK=1
    // the SD is a genuine FAT image, so the harvest/upload pipeline runs through
    // real FatFs and the corruption→degrade→reformat lifecycle is real. Host files
    // already in ./emu_sdcard[_internal] are imported so the drop-files workflow
    // still seeds the card.
    if (getenv("EMU_SD_BLOCK")) {
        // 16 GB card, 8 GB P1 (matches the device); FakeSd is sparse so memory
        // tracks only written sectors. Dual-partition FAT32 via sdFormatDual.
        s_blockFs = new FatFsFilesys(31116288, 16777216, SD_ROOT, SD_INTERNAL);
        if (s_blockFs->format()) {
            int n1 = s_blockFs->importHostTree(SD_ROOT, SD_ROOT);
            int n2 = s_blockFs->importHostTree(SD_INTERNAL, SD_INTERNAL);
            s_hal.filesys = s_blockFs;
            s_sdBlockMode = true;
            airbridge_log("SD: FakeSd block mode (FatFs on in-memory card) — imported %d+%d host files", n1, n2);
            printf("SD block mode: ON (real FatFs on FakeSd; 'B'/SIGUSR2 corrupts the FAT)\n");
        } else {
            delete s_blockFs; s_blockFs = nullptr;
            printf("SD block mode: f_mkfs FAILED — staying on host-directory backend\n");
        }
    }

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
    printf("Render: %s (use --render / EMU_RENDER=1 for the OLED window)\n", g_render ? "ON" : "OFF (headless)");
    if (g_render)
        printf("Keys: D=DSU-session  I=status  T=test-AT  C=toggle-net  X=drop-session  R=reset  Q=quit\n");
    printf("\n");
    fflush(stdout);
    setbuf(stdout, nullptr);  // disable buffering for real-time output
    s_log.clear();

    // Launch modem init in background (same AT sequence as real device)
    DisplayState ds = {};
    strlcpy(ds.modemOp, "Booting...", sizeof(ds.modemOp));
    std::thread modemThread(modemInitThread, &ds);

    // Timer subsystem works headless (no display required); video is opt-in.
    if (SDL_Init(SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init(TIMER): %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (g_render) {
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            fprintf(stderr, "SDL_InitSubSystem(VIDEO): %s — continuing headless\n", SDL_GetError());
        } else {
            window = SDL_CreateWindow(
                "AirBridge OLED Emulator (128x64)",
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                WIN_W, WIN_H, SDL_WINDOW_SHOWN);
            if (window) {
                renderer = SDL_CreateRenderer(window, -1,
                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                if (!renderer)
                    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
            } else {
                fprintf(stderr, "SDL_CreateWindow: %s — continuing headless\n", SDL_GetError());
            }
        }
    }

    // ── OTA check (like uploadTask does first after network) ────────
    // Wait for modem init in a non-blocking way
    static bool otaChecked = false;

    // Check for pending uploads from previous session (scan subfolders)
    {
        char harvestDir[256];
        snprintf(harvestDir, sizeof(harvestDir), "%s/upload", SD_INTERNAL);
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
        while (SDL_GetTicks() < splashEnd && running && !s_quit) {
            if (renderer) {
                SDL_Event ev;
                while (SDL_PollEvent(&ev)) { if (ev.type == SDL_QUIT) running = false; }
            }
            SDL_Delay(100);
        }
    }
    while (running && !s_quit) {
        SDL_Event event;
        // Interactive keypresses are debug-only (render mode); headless tests
        // drive the emulator via file drops + SIGUSR1/SIGTERM.
        while (renderer && SDL_PollEvent(&event)) {
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
                    printf("Harvesting from %s/ → %s/upload/ ...\n", SD_ROOT, SD_INTERNAL);
                    char destDir[256];
                    snprintf(destDir, sizeof(destDir), "%s/upload", SD_INTERNAL);
                    uint32_t hnum = 0;
                    g_hal->nvs->get_u32("harvest", "count", &hnum);
                    hnum++;
                    g_hal->nvs->set_u32("harvest", "count", hnum);
                    HarvestResult r = harvestFiles(SD_ROOT, destDir, (uint16_t)hnum, s_compress);
                    printf("Harvested: %u file(s), %.1f MB → %s\n", r.count, r.usedMb, r.folder);
                    modelGzipTime(r.inBytes);
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
                    atCmd("AT+CEREG?");
                    atCmd("AT+CGREG?");
                    atCmd("AT+CCLK?");
                    printf("[AT] Done\n");
                    break;
                }
                case SDLK_u:
                    ds.hostWrittenMb += 10.0f;
                    ds.usbWriteKBps = 5000.0f;
                    printf("USB write: +10MB\n");
                    break;
                case SDLK_b:  // "break" the SD: corrupt the FAT (block mode) — drives the recovery demo
                    if (s_sdBlockMode) s_sdCorrupt = 1;
                    else printf("[SD] block mode off — run with EMU_SD_BLOCK=1\n");
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
                    const char* dsuFile = getenv("EMU_DSU_FILE");
                    if (!dsuFile) dsuFile = "test/fixtures/EA500.000243_01077.eaofh";
                    printf("[DSU] Running download session from %s\n", dsuFile);
                    SimDSU dsu;
                    dsu.sdRoot = SD_ROOT;
                    dsu.internalMemoryPath = dsuFile;
                    dsu.metricsSource = "test/fixtures/metrics";
                    SimDSU::SessionResult sr = dsu.runSession();
                    if (sr.success && sr.bytesWritten > 0) {
                        printf("[DSU] Session complete: %s (flights %u-%u, %u bytes)\n",
                               sr.filename, sr.firstFlight, sr.flightNum, sr.bytesWritten);
                    } else if (sr.success) {
                        printf("[DSU] Caught up — no new flights\n");
                    } else {
                        printf("[DSU] Session failed (check EMU_DSU_FILE)\n");
                    }
                    break;
                }
                case SDLK_x:
                    // Simulate carrier-terminated PPP session (T-Mobile drops every ~4-6h)
                    if (s_modem && s_modem->pppUp) {
                        s_modem->dropSession();
                        printf("[SIM] Carrier session dropped — reconnect loop should fire\n");
                    } else {
                        printf("[SIM] No active PPP session to drop\n");
                    }
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
            if (getenv("EMU_PROF")) printf("[PROF] ota_check_start t=%ums\n", SDL_GetTicks());
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

            if (getenv("EMU_PROF")) printf("[PROF] cookie_start t=%ums\n", SDL_GetTicks());
            // Fetch S3 DSU cookie (same as firmware uploadTask)
            {
                char apiHost[128] = "", apiKey[64] = "";
                g_hal->nvs->get_str("s3", "api_host", apiHost, sizeof(apiHost));
                g_hal->nvs->get_str("s3", "api_key", apiKey, sizeof(apiKey));
                if (apiHost[0] && apiKey[0]) {
                    TlsHandle tls = g_hal->network->connect(apiHost);
                    if (tls) {
                        char devId[32] = "";
                        g_hal->nvs->get_str("s3", "device_id", devId, sizeof(devId));
                        char req[512];
                        snprintf(req, sizeof(req),
                            "GET /prod/firmware/cookie?device=%s HTTP/1.1\r\nHost: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
                            devId, apiHost, apiKey);
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

            // Per-aircraft S3 cookie override (date-mode or flight-mode, admin-pushed).
            // Stored at aircraft/{serial}/cookie.easdf — one-shot, deleted after fetch.
            {
                char cookiePath[256];
                snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", SD_ROOT);
                char acSerial[44] = "";
                void* cf = g_hal->filesys->open(cookiePath, "rb");
                if (cf) {
                    uint8_t ck[78] = {};
                    if (g_hal->filesys->read(cf, ck, 78) == 78 &&
                        ck[0] == 0xEA && ck[1] == 0x1E) {
                        size_t n = sizeof(acSerial) - 1; if (n > 42) n = 42;
                        memcpy(acSerial, ck + 9, n); acSerial[n] = '\0';
                        for (int j = (int)n - 1; j >= 0; j--) {
                            if ((uint8_t)acSerial[j] < 0x20 || (uint8_t)acSerial[j] > 0x7E)
                                acSerial[j] = '\0'; else break;
                        }
                    }
                    g_hal->filesys->close(cf);
                }
                if (!acSerial[0])
                    g_hal->nvs->get_str("mfst", "serial", acSerial, sizeof(acSerial));

                if (acSerial[0]) {
                    S3Creds creds = loadS3Creds();
                    if (creds.valid) {
                        char path[256];
                        snprintf(path, sizeof(path), "/prod/aircraft/cookie?serial=%s", acSerial);
                        std::string resp = s3ApiGetPathViaHal(creds.apiHost, creds.apiKey, path);
                        std::string hexStr = jsonStr(resp, "cookie");
                        if (hexStr.size() == 156) {
                            uint8_t cookie[78];
                            for (int i = 0; i < 78; i++) {
                                char h[3] = { hexStr[i*2], hexStr[i*2+1], 0 };
                                cookie[i] = (uint8_t)strtol(h, nullptr, 16);
                            }
                            if (cookie[0] == 0xEA && cookie[1] == 0x1E) {
                                void* wf = g_hal->filesys->open(cookiePath, "wb");
                                if (wf) {
                                    g_hal->filesys->write(wf, cookie, 78);
                                    g_hal->filesys->close(wf);
                                    printf("[S3] Aircraft cookie applied: %s\n", acSerial);
                                }
                            }
                        }
                    }
                }
            }

            // Manifest-based cookie sync: if S3 hwm > local cookie, advance cookie.
            // Handles AirBridge swap — ensures DSU won't re-download already-uploaded flights.
            {
                char cookiePath[256];
                snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", SD_ROOT);
                void* cf = g_hal->filesys->open(cookiePath, "rb");
                char cookieSerial[44] = "";
                uint32_t localFlight = 0;
                if (cf) {
                    uint8_t ck[78] = {};
                    if (g_hal->filesys->read(cf, ck, 78) == 78 &&
                        ck[0] == 0xEA && ck[1] == 0x1E) {
                        size_t copyLen = sizeof(cookieSerial) - 1;
                        if (copyLen > 42) copyLen = 42;
                        memcpy(cookieSerial, ck + 9, copyLen);
                        cookieSerial[copyLen] = '\0';
                        for (int j = (int)copyLen - 1; j >= 0; j--) {
                            if ((uint8_t)cookieSerial[j] < 0x20 || (uint8_t)cookieSerial[j] > 0x7E)
                                cookieSerial[j] = '\0';
                            else break;
                        }
                        localFlight = ((uint32_t)ck[62] << 24) | ((uint32_t)ck[63] << 16)
                                    | ((uint32_t)ck[64] << 8) | ck[65];
                        if (localFlight == 0xFFFFFFFF) { localFlight = 0; cookieSerial[0] = '\0'; }
                    }
                    g_hal->filesys->close(cf);
                }
                // Fall back to NVS manifest cache for serial
                if (!cookieSerial[0])
                    g_hal->nvs->get_str("mfst", "serial", cookieSerial, sizeof(cookieSerial));

                if (getenv("EMU_PROF")) printf("[PROF] manifest_start t=%ums\n", SDL_GetTicks());
                if (cookieSerial[0]) {
                    uint32_t s3Hwm = halFetchManifest(cookieSerial);
                    printf("[Boot] Manifest sync: %s local=%lu S3=%lu\n",
                           cookieSerial, (unsigned long)localFlight, (unsigned long)s3Hwm);
                    // Sync forward: write cookie when S3 HWM is higher than local.
                    // Admin backward rewind requires force_cookie (TODO).
                    if (s3Hwm > localFlight) {
                        uint8_t newCookie[78];
                        buildDsuCookie(cookieSerial, s3Hwm, newCookie);
                        void* wf = g_hal->filesys->open(cookiePath, "wb");
                        if (wf) {
                            g_hal->filesys->write(wf, newCookie, 78);
                            g_hal->filesys->close(wf);
                            printf("[Boot] Cookie synced to S3 hwm=%lu\n", (unsigned long)s3Hwm);
                        }
                    }
                }
            }
        }

        // Auto-upload pending files after modem connects
        static bool bootUploadChecked = false;
        if (!bootUploadChecked && s_modemInitDone && ds.pppConnected && ds.mbQueued > 0.0f && !s_uploading) {
            bootUploadChecked = true;
            if (getenv("EMU_PROF")) printf("[PROF] upload_start t=%ums\n", SDL_GetTicks());
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
                        if (strcmp(ent.name, "upload") == 0) continue;
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
                bool newFiles = false;
                if (lastFileCount < 0) {
                    // Boot scan: check for files left behind by a power loss
                    // (mirrors the firmware's app_main boot scan via shared
                    // hasUnharvestedFiles() in airbridge_harvest.h).
                    if (hasUnharvestedFiles(SD_ROOT)) {
                        printf("[Boot] Unharvested files on SD — scheduling harvest\n");
                        s_bootHarvestPending = true;  // bypass quiet window
                        newFiles = true;
                    }
                } else if (count > lastFileCount) {
                    // New files appeared while running — simulate USB write
                    printf("[USB] New files detected (%d → %d)\n", lastFileCount, count);
                    newFiles = true;
                }
                if (newFiles) {
                    s_writeDetected = true;
                    if (!s_bootHarvestPending) s_lastWriteMs = now;
                    s_hostWasConnected = true;
                    float newMb = 0;
                    void* d2 = g_hal->filesys->opendir(SD_ROOT);
                    if (d2) {
                        FsDirEntry e2;
                        while (g_hal->filesys->readdir(d2, &e2)) {
                            if (e2.name[0] == '.' || strcmp(e2.name, "upload") == 0) continue;
                            uint32_t sz = 0; bool isDir = false;
                            char fp[256]; snprintf(fp, sizeof(fp), "%s/%s", SD_ROOT, e2.name);
                            if (g_hal->filesys->stat(fp, &sz, &isDir) && !isDir)
                                newMb += sz / 1e6f;
                        }
                        g_hal->filesys->closedir(d2);
                    }
                    ds.hostWrittenMb = newMb;
                }
                lastFileCount = count;
            }
        }

        // Harvest trigger — uses shared shouldHarvest() (same logic as firmware)
        if (shouldHarvest(s_harvesting, s_writeDetected, s_hostWasConnected,
                          s_lastWriteMs, s_lastHarvestMs, s_harvestCoolMs, now, s_bootHarvestPending)) {
            if (s_bootHarvestPending)
                printf("[Harvest] Triggering (boot scan — no quiet window)\n");
            else
                printf("[Harvest] Triggering (%.1f MB written, %us idle)\n",
                       ds.hostWrittenMb, (now - s_lastWriteMs) / 1000);
            s_bootHarvestPending = false;
            s_harvesting = true;

            char destDir[256];
            snprintf(destDir, sizeof(destDir), "%s/upload", SD_INTERNAL);
            uint32_t hnum = 0;
            g_hal->nvs->get_u32("harvest", "count", &hnum);
            hnum++;
            g_hal->nvs->set_u32("harvest", "count", hnum);
            HarvestResult r = harvestFiles(SD_ROOT, destDir, (uint16_t)hnum, s_compress);
            printf("[Harvest] Done: %u file(s), %.1f MB → %s\n", r.count, r.usedMb, r.folder);
            modelGzipTime(r.inBytes);   // charge modeled on-device gzip CPU time

            // Harvest-integrity guard: we fired because the host wrote ds.hostWrittenMb,
            // so verify we recovered roughly that. If not (data not yet committed to the
            // SD, or a truncated/unreadable file), don't silently drop it — retry the
            // harvest a few times before giving up.
            static int s_harvestRetries = 0;
            uint32_t wroteKB = (uint32_t)(ds.hostWrittenMb * 1024.0f);
            uint32_t gotKB   = (uint32_t)(r.usedMb * 1024.0f);
            if (harvestLooksIncomplete(wroteKB, r.count, gotKB) && s_harvestRetries < 3) {
                s_harvestRetries++;
                printf("[Harvest] WARN: incomplete — wrote %uKB but harvested %u file(s)/%uKB; "
                       "retry %d/3 (data may not have committed)\n", wroteKB, r.count, gotKB,
                       s_harvestRetries);
                airbridge_log("HARVEST: incomplete wrote=%uKB got=%u/%uKB retry=%d",
                              wroteKB, r.count, gotKB, s_harvestRetries);
                s_harvesting = false;  // leave s_writeDetected set → re-fires after the quiet window
                s_lastWriteMs = now;   // re-arm the quiet window so the retry waits (commit time)
                continue;
            }
            s_harvestRetries = 0;
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

        // Periodic STATUS heartbeat — keeps the log ring buffer non-empty so the
        // 10s flush writes new content each cycle (mirrors firmware's STATUS log).
        {
            static uint32_t lastStatus = 0;
            if (now - lastStatus > 30000) {
                lastStatus = now;
                airbridge_log("[+%lus] STATUS ppp=%d queued=%.1fMB uploaded=%.1fMB",
                              (unsigned long)(now / 1000),
                              (int)ds.pppConnected, ds.mbQueued, ds.mbUploaded);
            }
        }

        // Periodic log flush to SD (every 10s) + S3 upload (every 30s)
        {
            static uint32_t lastFlush = 0;
            if (s_loglen > 0 && (now - lastFlush) > 10000) {
                airbridge_log_flush(emuLogPath);
                lastFlush = now;
            }

            // Upload log chunks to S3 via /prod/log/append
            static uint32_t lastUpload = 0;
            static long lastUploadPos = 0;
            static std::atomic<bool> logUploading{false};
            if (ds.pppConnected && !logUploading && (now - lastUpload) > 30000) {
                // Old logs are moved to SD root at boot and handled by the
                // harvest → upload pipeline. Only upload current session here.

                lastUpload = now;
                logUploading = true;
                // Spawn upload thread
                std::thread([&ds, emuLogPath, emuLogSession, &logUploading, &lastUploadPos]() {
                    char apiHost[128] = "", apiKey[64] = "", devId[32] = "";
                    g_hal->nvs->get_str("s3", "api_host", apiHost, sizeof(apiHost));
                    g_hal->nvs->get_str("s3", "api_key", apiKey, sizeof(apiKey));
                    g_hal->nvs->get_str("s3", "device_id", devId, sizeof(devId));

                    auto uploadChunk = [&](const char* session, const char* chunk, int len) {
                        TlsHandle tls = g_hal->network->connect(apiHost);
                        if (!tls) return false;
                        char hdr[512];
                        snprintf(hdr, sizeof(hdr),
                            "POST /prod/log/append?device=%s&session=%s HTTP/1.1\r\n"
                            "Host: %s\r\nx-api-key: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
                            devId, session, apiHost, apiKey, len);
                        bool ok = g_hal->network->write(tls, hdr, strlen(hdr)) &&
                                  g_hal->network->write(tls, chunk, len);
                        if (ok) halHttpReadResponse(tls);
                        g_hal->network->destroy(tls);
                        return ok;
                    };

                    // Upload new content from current session
                    FILE* f = fopen(emuLogPath, "r");
                    if (f) {
                        fseek(f, 0, SEEK_END);
                        long sz = ftell(f);
                        if (sz > lastUploadPos) {
                            fseek(f, lastUploadPos, SEEK_SET);
                            static char buf[8192];
                            int rd = fread(buf, 1, sizeof(buf), f);
                            if (rd > 0 && uploadChunk(emuLogSession, buf, rd)) {
                                lastUploadPos += rd;
                            }
                        }
                        fclose(f);
                    }
                    logUploading = false;
                }).detach();
            }
        }

        // SIGUSR1 from test suite: toggle cellular (same as 'C' keypress)
        if (s_cellularToggle) {
            s_cellularToggle = 0;
            ds.pppConnected = !ds.pppConnected;
            if (ds.pppConnected) { ds.modemRssi = 22; ds.modemReady = true; }
            printf("Cellular (SIGUSR1): %s\n", ds.pppConnected ? "ON" : "OFF");
        }

        // Scripted cellular profile (EMU_CELL_FILE): poll on mtime change, apply.
        if (!s_cellFilePath.empty()) {
            struct stat cst;
            if (::stat(s_cellFilePath.c_str(), &cst) == 0 && cst.st_mtime != s_cellFileMtime) {
                s_cellFileMtime = cst.st_mtime;
                FILE* cf = fopen(s_cellFilePath.c_str(), "r");
                if (cf) {
                    char line[32] = "";
                    if (fgets(line, sizeof(line), cf)) {
                        line[strcspn(line, "\r\n")] = 0;
                        applyCellState(line, ds, s_modem);
                    }
                    fclose(cf);
                }
            }
        }

        // ── Remote command channel (mirrors the firmware modem-task poll) ────
        // Poll the real test Lambda/S3 every 5s when PPP is up; run the fetched
        // airbridge.cmd text through the SAME shared executor (allowSdOps=false,
        // SD-independent) → ack → apply. Proves the channel end-to-end, including
        // remote format_sd recovery of a wedged SD.
        {
            static uint32_t lastCmdPoll = 0;
            if (ds.pppConnected && deviceId[0] && (now - lastCmdPoll) > 5000) {
                lastCmdPoll = now;
                static char cmdText[1024];
                if (halFetchCommands(deviceId, cmdText, sizeof(cmdText)) && cmdText[0]) {
                    printf("[CMD] remote command received: %s\n", cmdText);
                    airbridge_log("CMD: remote command received (%u bytes)", (unsigned)strlen(cmdText));
                    // dirs unused (allowSdOps=false skips dump_logs) — pass empty.
                    CmdRunResult cr = runCommandTextBuffer(
                        cmdText, /*runtimeOnly=*/false, "", "", "",
                        nullptr, 0, /*allowSdOps=*/false);
                    char ack[256];
                    snprintf(ack, sizeof(ack),
                        "{\"fw\":\"%s\",\"ran\":%s,\"format\":%s,\"reboot\":%s,\"compress\":%s}",
                        FW_VERSION, cr.ran?"true":"false", cr.format?"true":"false",
                        cr.reboot?"true":"false", g_compress?"true":"false");
                    halAckCommand(deviceId, ack);
                    s_compress = g_compress;  // bridge shared flag → emulator harvest flag
                    if (cr.format && s_blockFs) {
                        printf("[CMD] format_sd — reformatting (remote recovery)\n");
                        airbridge_log("CMD: format_sd — reformatting (remote)");
                        s_blockFs->format();
                        s_blockFs->importHostTree(SD_ROOT, SD_ROOT);
                        s_blockFs->importHostTree(SD_INTERNAL, SD_INTERNAL);
                        airbridge_log("SD: reformatted + reseeded — recovered (remote format_sd)");
                        printf("[CMD] reformatted + recovered\n");
                    }
                }
            }
        }

        // ── SD health (block mode): probe → degrade → reformat ──────────────
        // Mirrors the firmware's sd_health_check/sdRecoveryAction on the FakeSd-
        // backed card: a corrupt FAT is detected, the OLED shows SD ERROR, and a
        // persistent failure triggers a real reformat that recovers the card
        // (re-seeded from the host dirs = the DSU re-sending via the cookie).
        // Thresholds are compressed (degrade@2s, reformat@5s) for a watchable demo.
        if (s_sdBlockMode && s_blockFs) {
            static uint32_t healthStart = 0, lastHealth = 0;
            static SdHealthState health;   // SAME state machine the firmware runs
            static bool degraded = false, autoCorrupted = false;
            if (healthStart == 0) healthStart = now;
            const char* afterMs = getenv("EMU_SD_CORRUPT_AFTER_MS");
            if (afterMs && !autoCorrupted && (now - healthStart) >= (uint32_t)atoi(afterMs)) {
                autoCorrupted = true; s_sdCorrupt = 1;
            }
            if (s_sdCorrupt) {
                s_sdCorrupt = 0;
                airbridge_log("SD: TEST corruption injected (FakeSd FAT zeroed)");
                printf("[SD] FAT corruption injected\n");
                s_blockFs->corruptFat();
            }
            if (now - lastHealth >= 1000) {
                lastHealth = now;
                bool ok = s_blockFs->probeOk();
                if (ok && degraded) { degraded = false; ds.sdError = false; ds.sdReformatting = false; }
                // Compressed thresholds (degrade@2, reformat@5) for a watchable demo.
                SdRecoveryAction act = sdHealthUpdate(health, ok, 2, 5);
                if (act == SD_RECOVERY_DEGRADE && !degraded) {
                    degraded = true; ds.sdError = true; ds.sdReformatting = false;
                    airbridge_log("SD: runtime failure (n=%u) — degraded; SD ERROR on OLED, logging via cellular",
                                  (unsigned)health.consecutiveFailures);
                    printf("[SD] degraded — SD ERROR shown on OLED\n");
                } else if (act == SD_RECOVERY_REFORMAT) {
                    ds.sdError = true; ds.sdReformatting = true;
                    updateDisplay(ds); renderFramebuffer(renderer);
                    airbridge_log("SD: unrecoverable — reformatting (data re-sent by DSU via cookie)");
                    printf("[SD] reformatting...\n");
                    s_blockFs->format();
                    s_blockFs->importHostTree(SD_ROOT, SD_ROOT);
                    s_blockFs->importHostTree(SD_INTERNAL, SD_INTERNAL);
                    health = SdHealthState{}; degraded = false; ds.sdError = false; ds.sdReformatting = false;
                    airbridge_log("SD: reformatted + reseeded — recovered");
                    printf("[SD] recovered\n");
                }
            }
        }

        updateDisplay(ds);
        renderFramebuffer(renderer);
        SDL_Delay(100);
    }

    // Final flush before exit
    airbridge_log_flush(emuLogPath);

    if (mscThread.joinable()) {
        s_mscActive = false;
        mscThread.join();
        printf("SD contention stats: fsReads=%ld mscReads=%ld mscTimeouts=%ld\n",
               s_simSd.fsReads.load(), s_simSd.mscReads.load(), s_simSd.mscTimeouts.load());
        g_simSdCard() = nullptr;
    }
    if (modemThread.joinable()) modemThread.join();
    s_dsuStop = true;
    if (dsuThread.joinable()) dsuThread.join();
    // Clean up pppd and routing
    if (s_clientPppd > 0) {
        system("sudo ip rule del from 10.64.64.2 table 100 2>/dev/null");
        system("sudo ip route flush table 100 2>/dev/null");
        kill(s_clientPppd, SIGTERM);
        waitpid(s_clientPppd, nullptr, 0);
    }
    if (s_modem) { s_modem->stop(); delete s_modem; }
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

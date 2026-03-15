#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SdFat.h>
#include "USB.h"
#include "USBMSC.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ── Pin assignments ────────────────────────────────────────────────────────────
#define PIN_I2C_SCL  7
#define PIN_I2C_SDA  8
#define PIN_SD_CS   10
#define PIN_SD_MOSI 11
#define PIN_SD_MISO 12
#define PIN_SD_SCK  13

// ── Display ────────────────────────────────────────────────────────────────────
#define SCREEN_W   128
#define SCREEN_H    64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

#define DBG Serial

// ── SD card ───────────────────────────────────────────────────────────────────
// Single-partition design: whole card is one FAT32 volume (no MBR).
//
// setup(): sd.begin() detects/formats the FS.  MSC uses sd.card()->read/write
// which bypass sd's FS cache (so the cache is always stale during MSC).
//
// doHarvest(): takes g_sd_mutex (blocks uploadTask), calls sd.begin() again to
// get a fresh FS view, then uses sd.rename()/sd.exists()/sd.mkdir() directly.
// g_harvesting=true blocks all MSC callbacks so only one writer at a time.
//
// uploadTask(): uses sd.open()/sd.remove() with the mutex while g_harvesting=false.
// It only accesses /harvested/ which the host never writes, so the cache stays valid.
static SPIClass    spi(HSPI);
static SdFs        sd;
// SHARED_SPI: calls beginTransaction/endTransaction per operation,
// releasing the bus between calls.  Required for sd.begin() to work
// inside doHarvest() without re-acquiring a still-held DEDICATED bus lock.
static SdSpiConfig g_cfg(PIN_SD_CS, SHARED_SPI, SD_SCK_MHZ(10), &spi);

// ── SPI mutex ─────────────────────────────────────────────────────────────────
// Created BEFORE USB.begin() so TinyUSB callbacks never see nullptr.
static SemaphoreHandle_t g_sd_mutex = nullptr;
static volatile bool     g_sd_ready = false;

USBMSC MSC;

// ── Harvest timing ─────────────────────────────────────────────────────────────
#define QUIET_WINDOW_MS     30000UL
#define DISPLAY_INTERVAL_MS  1000UL

static volatile bool     g_hostConnected    = false;
static volatile bool     g_hostWasConnected = false;
static volatile uint32_t g_lastWriteMs      = 0;
static volatile bool     g_writeDetected    = false;
static volatile bool     g_harvesting       = false;

static uint16_t g_filesQueued    = 0;
static uint16_t g_filesUploaded  = 0;
static float    g_hostWrittenMb  = 0.0f;  // MB written by USB host since last harvest
static float    g_mbQueued       = 0.0f;  // MB in /harvested awaiting upload
static float    g_mbUploaded     = 0.0f;  // MB uploaded this session
static float    g_sdTotalMb      = 0.0f;
static uint32_t g_lastDisplayMs = 0;

static TaskHandle_t g_upload_task = nullptr;
static uint32_t     g_card_sectors = 0;

// Deferred harvest log — written during harvest, printed by loop() after serial reconnects
static char g_harvest_log[512] = "";

// ── MSC callbacks ──────────────────────────────────────────────────────────────
static int32_t msc_read(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
    if (!g_sd_ready || g_harvesting) return -1;
    (void)offset;
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    bool ok = sd.card()->readSectors(lba, (uint8_t*)buf, bufsize / 512);
    xSemaphoreGive(g_sd_mutex);
    return ok ? (int32_t)bufsize : -1;
}

static int32_t msc_write(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    if (!g_sd_ready || g_harvesting) return -1;
    (void)offset;
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    bool ok = sd.card()->writeSectors(lba, buf, bufsize / 512);
    xSemaphoreGive(g_sd_mutex);
    if (ok) {
        g_lastWriteMs      = millis();
        g_writeDetected    = true;
        g_hostWasConnected = true;
        g_hostWrittenMb   += bufsize / 1e6f;
    }
    return ok ? (int32_t)bufsize : -1;
}

static bool msc_start_stop(uint8_t, bool start, bool load_eject) {
    if (load_eject) {
        g_hostConnected = start;
        if (start) g_hostWasConnected = true;
    }
    return true;
}

// ── Display ────────────────────────────────────────────────────────────────────
static void disp(const char* line1, const char* line2 = nullptr) {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(14, 6);
    display.print("AirBridge");
    display.drawLine(0, 26, 127, 26, SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 32);
    display.print(line1);
    if (line2) { display.setCursor(0, 48); display.print(line2); }
    display.display();
}

static void _fmtSize(char* buf, size_t len, float mb) {
    if (mb >= 1000.0f)   snprintf(buf, len, "%.1fGB", mb / 1024.0f);
    else if (mb >= 0.1f) snprintf(buf, len, "%.1fMB", mb);
    else                 snprintf(buf, len, "%.1fKB", mb * 1024.0f);
}

static void updateDisplay() {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);

    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("NO CELL");
    // Signal bars (4 outline bars = no signal)
    const int8_t xs[4] = {108,113,118,123}, hs[4] = {2,4,6,8};
    for (int i = 0; i < 4; i++)
        display.drawRect(xs[i], 8-hs[i], 3, hs[i], SSD1306_WHITE);
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

    display.setTextSize(2);
    const char* lbl; int lx;
    if (g_harvesting)         { lbl = "HARVEST";    lx = 22; }
    else if (g_hostConnected) { lbl = "USB ACTIVE"; lx =  4; }
    else                      { lbl = "USB  IDLE";  lx = 10; }
    display.setCursor(lx, 12);
    display.print(lbl);

    // Row 36: USB storage bar — shows host-written bytes / total card capacity
    display.setTextSize(1);
    char sz[12]; _fmtSize(sz, sizeof(sz), g_hostWrittenMb);
    int sizeW = strlen(sz) * 6;
    int sizeX = 128 - sizeW;
    int barX  = 20;  // "USB" (3 chars × 6px = 18) + 2px gap
    int barW  = sizeX - 2 - barX;
    display.setCursor(0, 36);
    display.print("USB");
    display.setCursor(sizeX, 36);
    display.print(sz);
    if (barW > 2) {
        display.drawRect(barX, 36, barW, 7, SSD1306_WHITE);
        if (g_sdTotalMb > 0.0f && g_hostWrittenMb >= 0.001f) {
            int fill = (int)(g_hostWrittenMb / g_sdTotalMb * (barW - 2));
            if (fill > 0) display.fillRect(barX+1, 37, fill, 5, SSD1306_WHITE);
        }
    }

    // Row 50: Upload progress bar — "UP X.XMB [====] R X.XMB" (like Pi page 6)
    {
        char upStr[12], remStr[12];
        strcpy(upStr, "UP"); _fmtSize(upStr + 2, sizeof(upStr) - 2, g_mbUploaded);
        strcpy(remStr, "R"); _fmtSize(remStr + 1, sizeof(remStr) - 1, g_mbQueued);
        int upW  = strlen(upStr) * 6;
        int remW = strlen(remStr) * 6;
        int remX = 128 - remW;
        int ubX  = upW + 2;
        int ubW  = remX - 2 - ubX;
        display.setCursor(0, 50);
        display.print(upStr);
        if (ubW > 2) {
            display.drawRect(ubX, 50, ubW, 7, SSD1306_WHITE);
            float totalMb = g_mbUploaded + g_mbQueued;
            if (totalMb > 0.001f) {
                int fill = (int)(g_mbUploaded / totalMb * (ubW - 2));
                if (fill > 0) display.fillRect(ubX+1, 51, fill, 5, SSD1306_WHITE);
            }
        }
        display.setCursor(remX, 50);
        display.print(remStr);
    }

    display.display();
}

// ── Skip list ──────────────────────────────────────────────────────────────────
static const char* const SKIP_NAMES[] = {
    "System Volume Information", "desktop.ini", "Thumbs.db",
    ".Spotlight-V100", ".Trashes", ".fseventsd", "harvested", nullptr
};
static bool isSkipped(const char* n) {
    if (n[0] == '.' || n[0] == '~') return true;
    for (int i = 0; SKIP_NAMES[i]; i++)
        if (strcmp(n, SKIP_NAMES[i]) == 0) return true;
    return false;
}

// ── Upload task ────────────────────────────────────────────────────────────────
static void uploadTask(void* /*param*/) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        for (;;) {
            if (g_harvesting) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

            char name[64] = "";
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            {
                FsFile root, entry;
                root.open(&sd, "/harvested", O_RDONLY);
                while (entry.openNext(&root, O_RDONLY)) {
                    if (!entry.isDir()) { entry.getName(name, sizeof(name)); entry.close(); break; }
                    entry.close();
                }
                root.close();
            }
            xSemaphoreGive(g_sd_mutex);

            if (!name[0]) break;

            DBG.printf("Upload stub: %s\n", name);
            // TODO: read chunks with mutex, transmit via cellular

            char path[80];
            snprintf(path, sizeof(path), "/harvested/%s", name);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            float fileMb = 0.0f;
            { FsFile f; if (f.open(&sd, path, O_RDONLY)) { fileMb = (float)f.fileSize() / 1e6f; f.close(); } }
            bool removed = sd.remove(path);
            xSemaphoreGive(g_sd_mutex);
            if (removed) {
                if (g_filesQueued > 0) g_filesQueued--;
                g_filesUploaded++;
                g_mbUploaded += fileMb;
                if (g_mbQueued >= fileMb) g_mbQueued -= fileMb; else g_mbQueued = 0.0f;
            }
        }
        DBG.printf("Upload idle — %u uploaded\n", g_filesUploaded);
    }
}

// ── Harvest ────────────────────────────────────────────────────────────────────
static void doHarvest() {
    g_harvesting = true;
    delay(150);   // let any in-flight MSC sector transfer finish
    updateDisplay();

    // Take mutex to exclude uploadTask from the SD bus for the entire harvest.
    // MSC callbacks are already blocked by g_harvesting=true.
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);

    // sd.begin() re-initializes the card (CMD0/8/41) and re-mounts the FS.
    // After the host's raw MSC writes the card may be in power-save mode;
    // retry up to 3 times with a short delay to give it time to wake up.
    g_harvest_log[0] = '\0';
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = sd.begin(g_cfg);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "Harvest attempt %d: sd.begin=%s\n", i+1, ok ? "OK" : "FAIL");
        strncat(g_harvest_log, tmp, sizeof(g_harvest_log) - strlen(g_harvest_log) - 1);
        if (!ok) delay(500);
    }

    if (!ok) {
        xSemaphoreGive(g_sd_mutex);
        // Reset harvest state so we don't immediately retry in loop()
        g_writeDetected = false; g_lastWriteMs = 0;
        g_hostWasConnected = false; g_hostConnected = false;
        g_harvesting = false;
        return;
    }

    // Ensure /harvested directory exists
    if (!sd.exists("/harvested")) sd.mkdir("/harvested");

    FsFile root, entry;
    root.open(&sd, "/", O_RDONLY);
    uint16_t count = 0; float usedMb = 0.0f;

    while (entry.openNext(&root, O_RDONLY)) {
        char name[64];
        entry.getName(name, sizeof(name));
        if (entry.isDir() || entry.isHidden() || entry.isSystem() || isSkipped(name)) {
            entry.close(); continue;
        }
        float fileMb = (float)entry.fileSize() / 1e6f;
        entry.close();

        // Atomic rename: /name → /harvested/name
        char src[80], dst[80];
        snprintf(src, sizeof(src), "/%s", name);
        snprintf(dst, sizeof(dst), "/harvested/%s", name);
        if (!sd.exists(dst) && sd.rename(src, dst)) {
            DBG.printf("Harvested: %s (%.1f MB)\n", name, fileMb);
            usedMb += fileMb; count++;
        } else {
            DBG.printf("Rename failed or duplicate: %s\n", name);
        }
    }
    root.close();

    xSemaphoreGive(g_sd_mutex);

    g_filesQueued += count;
    if (count > 0) g_mbQueued += usedMb;
    DBG.printf("Harvest done: %u file(s) (%.1f MB)\n", count, usedMb);

    g_writeDetected = false; g_lastWriteMs = 0;
    g_hostWasConnected = false; g_hostConnected = false;
    g_hostWrittenMb = 0.0f;
    g_harvesting = false;

    if (count > 0 && g_upload_task) xTaskNotifyGive(g_upload_task);
}

// ── setup ─────────────────────────────────────────────────────────────────────
void setup() {
    g_sd_mutex = xSemaphoreCreateMutex();

    MSC.vendorID("AirBridg");
    MSC.productID("SD Storage");
    MSC.productRevision("1.0");
    MSC.onRead(msc_read);
    MSC.onWrite(msc_write);
    MSC.onStartStop(msc_start_stop);

    USB.begin();
    DBG.begin(115200);
    uint32_t t0 = millis();
    while (!DBG && (millis() - t0) < 3000) { delay(10); }
    DBG.println("USB up");

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        for (;;) { DBG.println("SSD1306 failed"); delay(1000); }
    }
    disp("Init SD...");

    spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    // sd.begin(): full init — card init (CMD0/8/41) + FAT volume mount.
    // If no valid filesystem, format FAT32 and retry.
    {
        bool has_fs = false;
        for (int i = 1; i <= 5 && !has_fs; i++) {
            has_fs = sd.begin(g_cfg);
            if (!has_fs) { disp("SD init...", "retrying"); delay(500); }
        }
        if (!has_fs) {
            DBG.println("No filesystem — formatting FAT32...");
            disp("Formatting...", "please wait");
            if (sd.format(&DBG)) {
                DBG.println("Format OK — remounting...");
                has_fs = sd.begin(g_cfg);
            }
        }
        if (!has_fs) {
            disp("SD init failed!", "Check wiring");
            for (;;) delay(1000);
        }
        g_card_sectors = sd.card()->sectorCount();
        DBG.printf("SD OK: %lu sectors (%.0f MB)\n",
                   g_card_sectors, g_card_sectors * 512.0f / 1e6f);
    }

    g_sdTotalMb = g_card_sectors * 512.0f / 1e6f;

    // MSC: raw sector access through sd.card() bypasses sd's FS cache.
    // sd.begin() in doHarvest() refreshes the cache each harvest cycle.
    MSC.begin(g_card_sectors, 512);
    MSC.mediaPresent(true);
    g_sd_ready = true;
    DBG.printf("MSC ready: %lu sectors (%.0f MB)\n", g_card_sectors, g_sdTotalMb);

    disp("USB drive ready", "");
    g_lastDisplayMs = millis();

    xTaskCreatePinnedToCore(uploadTask, "upload", 8192, nullptr, 1, &g_upload_task, 1);
    DBG.println("setup done");
}

void loop() {
    // Print buffered harvest log after serial reconnects (USB may have reset during harvest)
    if (g_harvest_log[0]) {
        DBG.print(g_harvest_log);
        g_harvest_log[0] = '\0';
    }

    uint32_t now = millis();
    if (now - g_lastDisplayMs >= DISPLAY_INTERVAL_MS) {
        g_lastDisplayMs = now;
        updateDisplay();
    }
    if (!g_harvesting && g_writeDetected && g_hostWasConnected &&
        g_lastWriteMs != 0 && (now - g_lastWriteMs) >= QUIET_WINDOW_MS) {
        DBG.println("Quiet window elapsed — triggering harvest");
        doHarvest();
    }
    delay(100);
}

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
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <lwip/sockets.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include "esp_mac.h"

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
static volatile uint32_t g_lastIoMs         = 0;      // last MSC read or write
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
static uint32_t g_lastHarvestMs  = 0;     // cooldown: prevent rapid re-harvest
static uint32_t g_harvestCoolMs = 30000;  // adaptive: 30s after files found, 5min after empty

static TaskHandle_t g_upload_task  = nullptr;
static TaskHandle_t g_harvest_task = nullptr;
static uint32_t     g_card_sectors = 0;

// Deferred harvest log — written during harvest, printed by loop() after serial reconnects
static char g_harvest_log[512] = "";

// ── WiFi / captive portal ─────────────────────────────────────────────────────
#define WIFI_AP_SSID            "AirBridge"
#define WIFI_CONNECT_TIMEOUT_MS  10000UL   // per-network connection attempt
#define WIFI_GRACE_MS            60000UL   // time disconnected before starting AP
#define AP_RETRY_MS             300000UL   // STA retry interval while in AP mode
#define MAX_KNOWN_NETS                  5

static volatile bool g_netConnected = false;   // true = STA associated + IP
static volatile bool g_apMode       = false;   // true = softAP running
static char          g_wifiLabel[22] = "No WiFi";
static int8_t        g_wifiBars      = 0;      // 0-4 filled signal bars

static volatile bool g_portal_connected = false;  // set by POST handler on success
static TaskHandle_t  g_wifi_task        = nullptr;

static DNSServer g_dns;
static WebServer g_http(80);

// Cached WiFi scan results (refreshed at AP start and on /scan request)
#define MAX_SCAN_RESULTS 15
static char   g_scan_ssids[MAX_SCAN_RESULTS][33];
static int8_t g_scan_rssi[MAX_SCAN_RESULTS];
static bool   g_scan_enc[MAX_SCAN_RESULTS];
static int    g_scan_count = 0;

// ── MSC callbacks ──────────────────────────────────────────────────────────────
static int32_t msc_read(uint32_t lba, uint32_t offset, void* buf, uint32_t bufsize) {
    if (!g_sd_ready || g_harvesting) return -1;
    (void)offset;
    // TinyUSB task stack is only 4 KB — xSemaphoreTake adds ~200 bytes.
    // Use a short timeout: if another task holds the mutex, return error
    // rather than blocking the USB task indefinitely.
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    bool ok = sd.card()->readSectors(lba, (uint8_t*)buf, bufsize / 512);
    xSemaphoreGive(g_sd_mutex);
    if (ok) g_lastIoMs = millis();
    return ok ? (int32_t)bufsize : -1;
}

static int32_t msc_write(uint32_t lba, uint32_t offset, uint8_t* buf, uint32_t bufsize) {
    if (!g_sd_ready || g_harvesting) return -1;
    (void)offset;
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    bool ok = sd.card()->writeSectors(lba, buf, bufsize / 512);
    xSemaphoreGive(g_sd_mutex);
    if (ok) {
        g_lastWriteMs      = millis();
        g_lastIoMs         = g_lastWriteMs;
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

// ── Portal HTML (dynamically built with scan results) ─────────────────────────

// Scan for nearby networks while AP stays up (WIFI_AP_STA mode).
static void doScan() {
    DBG.println("WiFi: scanning...");
    WiFi.mode(WIFI_AP_STA);   // AP stays up; STA radio scans
    int n = WiFi.scanNetworks(false, false);
    g_scan_count = (n > 0) ? min(n, MAX_SCAN_RESULTS) : 0;
    for (int i = 0; i < g_scan_count; i++) {
        strlcpy(g_scan_ssids[i], WiFi.SSID(i).c_str(), sizeof(g_scan_ssids[i]));
        g_scan_rssi[i] = (int8_t)constrain(WiFi.RSSI(i), -128, 0);
        g_scan_enc[i]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    }
    WiFi.scanDelete();
    DBG.printf("WiFi: scan found %d networks\n", g_scan_count);
}

static String buildPortalHTML() {
    String h;
    h.reserve(2048);
    h = "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AirBridge WiFi Setup</title><style>"
        "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px}"
        "h2{color:#333}label{color:#555;font-size:13px;display:block;margin-top:10px}"
        "select,input{display:block;width:100%;padding:10px;margin:4px 0;"
        "box-sizing:border-box;font-size:15px;border:1px solid #ccc;border-radius:4px}"
        "button{width:100%;padding:12px;background:#0078d4;color:#fff;border:none;"
        "font-size:16px;border-radius:4px;cursor:pointer;margin-top:12px}"
        ".rescan{display:block;text-align:center;margin-top:14px;color:#0078d4;font-size:14px}"
        "</style></head><body>"
        "<h2>AirBridge WiFi Setup</h2>"
        "<form method='POST' action='/configure'>";

    if (g_scan_count > 0) {
        h += "<label>Select network:</label>"
             "<select name='ssid'><option value=''>-- Choose network --</option>";
        for (int i = 0; i < g_scan_count; i++) {
            // Signal bars: █ (U+2588) filled, ░ (U+2591) empty
            int bars = (g_scan_rssi[i] >= -55) ? 4 :
                       (g_scan_rssi[i] >= -67) ? 3 :
                       (g_scan_rssi[i] >= -80) ? 2 : 1;
            String ssid = g_scan_ssids[i];
            ssid.replace("'", "&#39;");   // escape for HTML attribute
            h += "<option value='"; h += ssid; h += "'>";
            h += ssid; h += "  ";
            for (int b = 0; b < 4; b++)
                h += (b < bars) ? "\xe2\x96\x88" : "\xe2\x96\x91";  // █ or ░
            if (g_scan_enc[i]) h += " \xf0\x9f\x94\x92";  // 🔒
            h += "</option>";
        }
        h += "</select>"
             "<label>Or enter name manually (hidden network):</label>"
             "<input name='ssid_manual' placeholder='Network name' "
             "autocorrect='off' autocapitalize='off'>";
    } else {
        h += "<label>Network name:</label>"
             "<input name='ssid_manual' placeholder='WiFi Network Name' required "
             "autocorrect='off' autocapitalize='off'>"
             "<p style='color:#c00;font-size:13px'>No networks found — "
             "<a href='/scan'>scan again</a></p>";
    }

    h += "<label>Password:</label>"
         "<input name='password' type='password' placeholder='Password (leave blank if open)'>"
         "<button type='submit'>Connect</button>"
         "</form>"
         "<a class='rescan' href='/scan'>&#x1F504; Scan again</a>"
         "</body></html>";
    return h;
}

static const char SUCCESS_HTML[] = R"rawliteral(<!DOCTYPE html><html>
<head><meta charset="utf-8"><title>AirBridge</title></head>
<body><h2>Connected!</h2>
<p>AirBridge has joined your WiFi network. You can close this page.</p>
</body></html>)rawliteral";

static String buildErrorHTML(const String& ssid) {
    String h;
    h.reserve(512);
    h = "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AirBridge</title><style>"
        "body{font-family:sans-serif;max-width:400px;margin:40px auto;padding:0 20px}"
        "h2{color:#c00}.back{display:block;text-align:center;margin-top:20px;"
        "color:#0078d4;font-size:15px;text-decoration:none}"
        "</style></head><body>"
        "<h2>Connection failed</h2>"
        "<p>Could not connect to <strong>";
    h += ssid;
    h += "</strong>. Please check the password and try again.</p>"
         "<a class='back' href='/'>&#8592; Try again</a>"
         "</body></html>";
    return h;
}

// ── WiFi credential storage (NVS namespace "wifi") ────────────────────────────
struct NetCred { char ssid[33]; char pass[65]; };

static int loadKnownNets(NetCred* out) {
    Preferences p; p.begin("wifi", true);
    int n = min((int)p.getInt("count", 0), (int)MAX_KNOWN_NETS);
    for (int i = 0; i < n; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        strlcpy(out[i].ssid, p.getString(ks, "").c_str(), sizeof(out[i].ssid));
        strlcpy(out[i].pass, p.getString(kp, "").c_str(), sizeof(out[i].pass));
    }
    p.end(); return n;
}

// Saves network MRU-first; de-dupes by SSID.
static void saveNetwork(const char* ssid, const char* pass) {
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    // Remove existing entry for this SSID
    int j = 0;
    for (int i = 0; i < n; i++) if (strcmp(nets[i].ssid, ssid) != 0) nets[j++] = nets[i];
    n = j;
    // Prepend
    if (n >= MAX_KNOWN_NETS) n = MAX_KNOWN_NETS - 1;
    memmove(&nets[1], &nets[0], sizeof(NetCred) * n);
    strlcpy(nets[0].ssid, ssid, sizeof(nets[0].ssid));
    strlcpy(nets[0].pass, pass, sizeof(nets[0].pass));
    n++;
    Preferences p; p.begin("wifi", false);
    p.putInt("count", n);
    for (int i = 0; i < n; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        p.putString(ks, nets[i].ssid);
        p.putString(kp, nets[i].pass);
    }
    p.end();
    DBG.printf("WiFi: saved '%s' (%d stored)\n", ssid, n);
}

// ── WiFi helpers ──────────────────────────────────────────────────────────────
static int8_t rssiToBars(int32_t rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -80) return 2;
    if (rssi >= -90) return 1;
    return 0;
}

// Try connecting to one SSID; returns true if WL_CONNECTED within timeout.
static bool tryConnect(const char* ssid, const char* pass) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass[0] ? pass : nullptr);
    uint32_t t0 = millis();
    while (millis() - t0 < WIFI_CONNECT_TIMEOUT_MS) {
        if (WiFi.status() == WL_CONNECTED) return true;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    WiFi.disconnect(true);
    return false;
}

// Try all stored networks in MRU order.
static bool tryKnownNets() {
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    for (int i = 0; i < n; i++) {
        DBG.printf("WiFi: trying '%s'\n", nets[i].ssid);
        strlcpy(g_wifiLabel, nets[i].ssid, sizeof(g_wifiLabel));
        if (tryConnect(nets[i].ssid, nets[i].pass)) return true;
    }
    return false;
}

// ── Captive portal AP ─────────────────────────────────────────────────────────
static bool g_web_registered = false;

static void startAP() {
    WiFi.disconnect(true);
    vTaskDelay(pdMS_TO_TICKS(200));
    // WIFI_AP_STA: AP stays up while STA radio scans
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(WIFI_AP_SSID);
    vTaskDelay(pdMS_TO_TICKS(100));
    g_dns.start(53, "*", WiFi.softAPIP());

    // Scan for nearby networks now that the AP is up
    doScan();

    if (!g_web_registered) {
        // Serve dynamic portal page (includes scan results)
        auto servePage = []() { g_http.send(200, "text/html", buildPortalHTML()); };
        g_http.on("/", HTTP_GET, servePage);
        // /scan: rescan and redirect back to portal
        g_http.on("/scan", HTTP_GET, []() {
            doScan();
            g_http.sendHeader("Location", "/");
            g_http.send(302, "text/plain", "");
        });
        g_http.on("/configure", HTTP_POST, []() {
            // Prefer manually typed name (hidden networks); fall back to select value
            String ssid = g_http.arg("ssid_manual");
            if (ssid.isEmpty()) ssid = g_http.arg("ssid");
            String pass = g_http.arg("password");
            if (ssid.isEmpty()) {
                g_http.send(400, "text/plain", "Missing SSID");
                return;
            }
            // Test credentials inline while AP stays up (WIFI_AP_STA keeps softAP running)
            DBG.printf("Portal: testing '%s'...\n", ssid.c_str());
            WiFi.begin(ssid.c_str(), pass.isEmpty() ? nullptr : pass.c_str());
            bool connected = false;
            for (int i = 0; i < 50; i++) {  // up to 10 s (50 × 200 ms)
                if (WiFi.status() == WL_CONNECTED) { connected = true; break; }
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (connected) {
                saveNetwork(ssid.c_str(), pass.c_str());
                g_http.send(200, "text/html", SUCCESS_HTML);
                g_portal_connected = true;
            } else {
                WiFi.disconnect(false);  // drop STA attempt; keep AP running
                g_http.send(200, "text/html", buildErrorHTML(ssid));
            }
        });
        // Captive portal probe URLs — iOS and Android trigger the sign-in popup
        // when they don't get the expected response from these well-known paths.
        g_http.on("/hotspot-detect.html",       HTTP_GET, servePage);  // iOS
        g_http.on("/library/test/success.html", HTTP_GET, servePage);  // iOS newer
        g_http.on("/generate_204",              HTTP_GET, servePage);  // Android
        g_http.on("/connectivitycheck.html",    HTTP_GET, servePage);  // Android
        g_http.onNotFound([]() {
            g_http.sendHeader("Location", "http://192.168.4.1/");
            g_http.send(302, "text/plain", "");
        });
        g_web_registered = true;
    }
    g_http.begin();
    g_apMode = true;
    strlcpy(g_wifiLabel, WIFI_AP_SSID, sizeof(g_wifiLabel));
    g_wifiBars = 0;
    DBG.println("WiFi: AP started — SSID=AirBridge IP=192.168.4.1");
}

static void stopAP() {
    g_http.stop();
    g_dns.stop();
    WiFi.softAPdisconnect(true);
    g_apMode = false;
    DBG.println("WiFi: AP stopped");
}

// ── WiFi task — mirrors Pi upload_worker WiFi/portal state machine ─────────────
static void wifiTask(void* /*param*/) {
    vTaskDelay(pdMS_TO_TICKS(2000));  // let USB/SD settle first
    WiFi.persistent(false);           // we manage NVS ourselves via Preferences
    WiFi.setAutoReconnect(false);

    enum WState { WS_TRY_KNOWN, WS_CONNECTED, WS_AP };
    WState   state     = WS_TRY_KNOWN;
    uint32_t discMs    = 0;   // millis() when WiFi first lost
    uint32_t apRetryMs = 0;   // millis() of last STA scan from AP mode

    for (;;) {
        switch (state) {

        // ── Try every stored network (MRU order) ──────────────────────────────
        case WS_TRY_KNOWN: {
            strlcpy(g_wifiLabel, "Connecting...", sizeof(g_wifiLabel));
            g_netConnected = false;
            if (tryKnownNets()) {
                state = WS_CONNECTED; discMs = 0;
            } else {
                if (discMs == 0) discMs = millis();
                strlcpy(g_wifiLabel, "No WiFi", sizeof(g_wifiLabel));
                g_wifiBars = 0;
                if (millis() - discMs >= WIFI_GRACE_MS) {
                    state = WS_AP;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            }
            break;
        }

        // ── Maintain STA connection ────────────────────────────────────────────
        case WS_CONNECTED: {
            if (WiFi.status() == WL_CONNECTED) {
                if (!g_netConnected) {
                    DBG.printf("WiFi: connected to '%s' IP=%s\n",
                        WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
                }
                g_netConnected = true; discMs = 0;
                strlcpy(g_wifiLabel, WiFi.SSID().c_str(), sizeof(g_wifiLabel));
                g_wifiBars = rssiToBars(WiFi.RSSI());
                vTaskDelay(pdMS_TO_TICKS(5000));
            } else {
                g_netConnected = false;
                if (discMs == 0) { discMs = millis(); DBG.println("WiFi: connection lost"); }
                strlcpy(g_wifiLabel, "No WiFi", sizeof(g_wifiLabel));
                g_wifiBars = 0;
                state = WS_TRY_KNOWN;
            }
            break;
        }

        // ── AP + captive portal ────────────────────────────────────────────────
        case WS_AP: {
            if (!g_apMode) { startAP(); apRetryMs = millis(); }
            g_dns.processNextRequest();
            g_http.handleClient();

            // Portal POST handler connected successfully?
            if (g_portal_connected) {
                g_portal_connected = false;
                DBG.printf("WiFi: connected via portal to '%s'\n", WiFi.SSID().c_str());
                stopAP();
                state = WS_CONNECTED; discMs = 0;
                break;
            }

            // Periodic STA retry while in AP mode (every 5 min, like Pi STA_RETRY_IN_AP)
            if (millis() - apRetryMs >= AP_RETRY_MS) {
                DBG.println("WiFi: AP-mode STA retry");
                stopAP();
                vTaskDelay(pdMS_TO_TICKS(300));
                if (tryKnownNets()) {
                    state = WS_CONNECTED; discMs = 0;
                } else {
                    state = WS_TRY_KNOWN;  // will re-enter AP immediately (discMs already set)
                }
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(10));  // tight loop needed for DNS/HTTP responsiveness
            break;
        }
        }
    }
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
    // WiFi SSID / "AirBridge" (AP) / "No WiFi" — max 17 chars before the bars
    char label[18]; strlcpy(label, g_wifiLabel, sizeof(label));
    display.print(label);
    // Signal bars: filled up to g_wifiBars (0-4), rest outlined
    const int8_t xs[4] = {108,113,118,123}, hs[4] = {2,4,6,8};
    for (int i = 0; i < 4; i++) {
        if (i < g_wifiBars) display.fillRect(xs[i], 8-hs[i], 3, hs[i], SSD1306_WHITE);
        else                 display.drawRect(xs[i], 8-hs[i], 3, hs[i], SSD1306_WHITE);
    }
    display.drawLine(0, 9, 127, 9, SSD1306_WHITE);

    display.setTextSize(2);
    const char* lbl; int lx;
    if (g_harvesting)                                      { lbl = "HARVEST";    lx = 22; }
    else if (g_lastIoMs && millis() - g_lastIoMs < 2000)   { lbl = "USB ACTIVE"; lx =  4; }
    else if (g_writeDetected)                              { lbl = "USB  IDLE";  lx = 10; }
    else                                                   { lbl = "USB READY";  lx = 10; }
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

// ── S3 upload via pre-signed URLs ─────────────────────────────────────────────
// A Lambda backend generates time-limited S3 PUT URLs.  The ESP32 just does
// HTTPS PUT — no AWS credentials on device, no SigV4 signing needed.
//
// NVS namespace "s3": api_host, api_key  (device_id auto-generated from MAC)
// NVS namespace "s3up": upload session state for power-loss resume
//
// Small files (<5 MB): single pre-signed PUT
// Large files: S3 multipart — one pre-signed URL per 5 MB part

#define S3_CHUNK_SIZE (5UL * 1024 * 1024)  // 5 MB per part (S3 multipart minimum)

static char g_apiHost[128] = "";   // API Gateway hostname
static char g_apiKey[64]   = "";   // API Gateway API key
static char g_deviceId[16] = "";   // MAC-derived device identifier
// Load S3 API credentials from NVS into globals.
static bool s3LoadCreds() {
    if (g_apiHost[0] && g_apiKey[0] && g_deviceId[0]) return true;
    Preferences p; p.begin("s3", true);
    strlcpy(g_apiHost, p.getString("api_host", "").c_str(), sizeof(g_apiHost));
    strlcpy(g_apiKey,  p.getString("api_key", "").c_str(),  sizeof(g_apiKey));
    strlcpy(g_deviceId, p.getString("device_id", "").c_str(), sizeof(g_deviceId));
    p.end();
    if (!g_apiHost[0] || !g_apiKey[0]) {
        DBG.println("S3: no credentials in NVS");
        return false;
    }
    return true;
}

// Read HTTP response body, skipping headers.  Returns body as String.
// Also extracts the ETag header if etag buffer is provided.
// Handles chunked transfer encoding by stripping chunk-size lines.
static String httpReadResponse(WiFiClientSecure& tls, char* etag = nullptr, size_t etagSz = 0) {
    bool chunked = false;
    while (tls.connected()) {
        String line = tls.readStringUntil('\n');
        if (line.indexOf("chunked") >= 0) chunked = true;
        // Capture ETag header if requested (strip surrounding quotes)
        if (etag && etagSz > 0 && line.startsWith("ETag:")) {
            String val = line.substring(5);
            val.trim();
            // S3 returns ETags in quotes: "abc123" — strip them for clean JSON
            if (val.startsWith("\"") && val.endsWith("\""))
                val = val.substring(1, val.length() - 1);
            strlcpy(etag, val.c_str(), etagSz);
        }
        if (line == "\r") break;
    }
    String raw = "";
    uint32_t t1 = millis();
    while (millis() - t1 < 10000) {
        if (tls.available()) { raw += tls.readString(); t1 = millis(); }
        else if (!tls.connected()) break;
        else vTaskDelay(pdMS_TO_TICKS(50));
    }
    tls.stop();
    if (!chunked) return raw;
    // De-chunk: extract data between chunk-size lines
    String body = "";
    int pos = 0;
    while (pos < (int)raw.length()) {
        int nl = raw.indexOf('\n', pos);
        if (nl < 0) break;
        String szLine = raw.substring(pos, nl);
        szLine.trim();
        unsigned long chunkSz = strtoul(szLine.c_str(), nullptr, 16);
        if (chunkSz == 0) break;
        int dataStart = nl + 1;
        body += raw.substring(dataStart, dataStart + (int)chunkSz);
        pos = dataStart + (int)chunkSz + 2;
    }
    return body;
}

// Enlarge the TCP send buffer on a connected WiFiClientSecure socket.
// The default lwIP SO_SNDBUF is 5760 which caps throughput at ~94 KB/s.
static void enlargeSendBuffer(WiFiClientSecure& tls, int size = 32768) {
    int fd = tls.fd();
    if (fd >= 0) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
    }
}

// Stream `len` bytes from an open FsFile at current position to a TLS connection.
// Returns true if all bytes sent.
static bool httpStreamChunk(WiFiClientSecure& tls, FsFile& f, uint32_t len) {
    static uint8_t cbuf[8192];  // static: keep off 16 KB task stack
    uint32_t remaining = len;
    while (remaining > 0) {
        uint32_t toRead = (remaining < sizeof(cbuf)) ? remaining : sizeof(cbuf);
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        int n = f.read(cbuf, toRead);
        xSemaphoreGive(g_sd_mutex);
        if (n <= 0) return false;

        size_t sent = 0;
        uint32_t lastProgress = millis();
        while (sent < (size_t)n) {
            if (!tls.connected()) return false;
            if (millis() - lastProgress > 60000) return false;
            size_t wr = tls.write(cbuf + sent, n - sent);
            if (wr > 0) { sent += wr; lastProgress = millis(); }
            else vTaskDelay(pdMS_TO_TICKS(5));
        }
        remaining -= n;
    }
    return true;
}

// Clear stored upload session from NVS.
static void s3ClearSession() {
    Preferences p; p.begin("s3up", false);
    p.clear();
    p.end();
}

// Extract a JSON string value for a given key (no ArduinoJson dependency).
// Returns empty String if key not found.
static String jsonStr(const String& json, const char* key) {
    int pos = json.indexOf(key);
    if (pos < 0) return "";
    int colon = json.indexOf(':', pos);
    if (colon < 0) return "";
    // Find opening quote of value (skip whitespace/null)
    int q1 = json.indexOf('"', colon + 1);
    if (q1 < 0) {
        // Value might be null or a number — check for null
        int npos = json.indexOf("null", colon + 1);
        if (npos >= 0 && npos < colon + 10) return "";
        return "";
    }
    int q2 = json.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    return json.substring(q1 + 1, q2);
}

// Extract a JSON integer value for a given key.
static int jsonInt(const String& json, const char* key) {
    int pos = json.indexOf(key);
    if (pos < 0) return -1;
    int colon = json.indexOf(':', pos);
    if (colon < 0) return -1;
    return json.substring(colon + 1).toInt();
}

// Parse a URL into host and path+query components.
// E.g. "https://bucket.s3.us-west-2.amazonaws.com/key?params"
//   → host = "bucket.s3.us-west-2.amazonaws.com"
//   → path = "/key?params"
static bool parseUrl(const String& url, char* host, size_t hostSz,
                     char* path, size_t pathSz) {
    int schemeEnd = url.indexOf("://");
    if (schemeEnd < 0) return false;
    int hostStart = schemeEnd + 3;
    int pathStart = url.indexOf('/', hostStart);
    if (pathStart < 0) {
        strlcpy(host, url.substring(hostStart).c_str(), hostSz);
        strlcpy(path, "/", pathSz);
    } else {
        url.substring(hostStart, pathStart).toCharArray(host, hostSz);
        strlcpy(path, url.substring(pathStart).c_str(), pathSz);
    }
    return true;
}

// Make an HTTPS GET request to the API Gateway presign endpoint.
static String s3ApiGet(const char* queryParams) {
    WiFiClientSecure tls; tls.setInsecure();
    if (!tls.connect(g_apiHost, 443)) {
        DBG.printf("S3: TLS connect failed to %s\n", g_apiHost);
        return "";
    }
    tls.printf("GET /prod/presign?%s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "x-api-key: %s\r\n"
               "Connection: close\r\n\r\n",
               queryParams, g_apiHost, g_apiKey);
    return httpReadResponse(tls);
}

// Make an HTTPS POST to the API Gateway /complete endpoint.
static bool s3ApiComplete(const char* uploadId, const char* key,
                          const char* partsJson) {
    WiFiClientSecure tls; tls.setInsecure();
    if (!tls.connect(g_apiHost, 443)) {
        DBG.println("S3: TLS connect failed (complete)");
        return false;
    }
    // Build JSON body
    char body[2048];
    snprintf(body, sizeof(body),
        "{\"upload_id\":\"%s\",\"key\":\"%s\",\"parts\":[%s]}",
        uploadId, key, partsJson);
    int bodyLen = strlen(body);

    tls.printf("POST /prod/complete HTTP/1.1\r\n"
               "Host: %s\r\n"
               "x-api-key: %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: %d\r\n"
               "Connection: close\r\n\r\n",
               g_apiHost, g_apiKey, bodyLen);
    tls.print(body);

    String resp = httpReadResponse(tls);
    bool ok = resp.indexOf("\"ok\"") >= 0;
    if (!ok) DBG.printf("S3: complete failed: %s\n", resp.substring(0, 200).c_str());
    return ok;
}

// Upload a file from /harvested/<name> to S3 using pre-signed URLs.
// Small files (<5 MB): single PUT.  Large files: multipart with resume.
static bool s3UploadFile(const char* name) {
    if (!g_netConnected) { DBG.println("S3: no WiFi"); return false; }
    if (!s3LoadCreds()) return false;

    // Open file
    char fpath[80]; snprintf(fpath, sizeof(fpath), "/harvested/%s", name);
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FsFile f; bool fok = f.open(&sd, fpath, O_RDONLY);
    uint32_t fileSize = fok ? f.fileSize() : 0;
    xSemaphoreGive(g_sd_mutex);
    if (!fok || fileSize == 0) { DBG.printf("S3: can't open %s\n", fpath); return false; }

    static char s3Path[2500];  // shared by single-PUT and multipart paths

    // ── Small file: single pre-signed PUT ────────────────────────────────────
    if (fileSize <= S3_CHUNK_SIZE) {
        // Request a single pre-signed PUT URL
        char query[256];
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s", name, fileSize, g_deviceId);
        String resp = s3ApiGet(query);
        String url = jsonStr(resp, "\"url\"");
        if (!url.length()) {
            DBG.printf("S3: presign failed: %s\n", resp.substring(0, 200).c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Parse URL into host + path
        char s3Host[128];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            DBG.println("S3: URL parse failed");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // HTTPS PUT
        WiFiClientSecure tls; tls.setInsecure();
        if (!tls.connect(s3Host, 443)) {
            DBG.printf("S3: TLS connect failed to %s\n", s3Host);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        enlargeSendBuffer(tls);

        uint32_t xfrStart = millis();
        tls.printf("PUT %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   s3Path, s3Host, fileSize);

        if (!httpStreamChunk(tls, f, fileSize)) {
            DBG.println("S3: stream failed");
            tls.stop();
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);

        // Check response
        String putResp = httpReadResponse(tls);
        float elapsed = (millis() - xfrStart) / 1000.0f;
        DBG.printf("S3: uploaded '%s' OK (%u bytes, %.0f KB/s)\n", name, fileSize,
                   elapsed > 0 ? fileSize / 1024.0f / elapsed : 0);
        return true;
    }

    // ── Large file: multipart upload ─────────────────────────────────────────
    char uploadId[256] = "";
    char s3Key[128] = "";
    uint32_t startPart = 1;
    uint32_t totalParts = 0;

    // Check NVS for interrupted session
    {
        Preferences p; p.begin("s3up", true);
        String storedName = p.getString("name", "");
        if (storedName == name) {
            strlcpy(uploadId, p.getString("uid", "").c_str(), sizeof(uploadId));
            strlcpy(s3Key, p.getString("key", "").c_str(), sizeof(s3Key));
            startPart = p.getUInt("part", 1);
            totalParts = p.getUInt("parts", 0);
        }
        p.end();
    }

    // Start new multipart upload if no session
    if (!uploadId[0]) {
        char query[256];
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s", name, fileSize, g_deviceId);
        String resp = s3ApiGet(query);

        String uid = jsonStr(resp, "\"upload_id\"");
        String key = jsonStr(resp, "\"key\"");
        totalParts = jsonInt(resp, "\"parts\"");

        if (!uid.length() || !key.length() || totalParts <= 0) {
            DBG.printf("S3: multipart start failed: %s\n", resp.substring(0, 200).c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        strlcpy(uploadId, uid.c_str(), sizeof(uploadId));
        strlcpy(s3Key, key.c_str(), sizeof(s3Key));
        startPart = 1;

        // Persist session
        Preferences p; p.begin("s3up", false);
        p.putString("name", name);
        p.putString("uid", uploadId);
        p.putString("key", s3Key);
        p.putUInt("part", 1);
        p.putUInt("parts", totalParts);
        p.putUInt("size", fileSize);
        p.end();

        DBG.printf("S3: multipart started, %u parts, upload_id=%s\n",
                   totalParts, uploadId);
    } else {
        DBG.printf("S3: resuming multipart at part %u/%u\n", startPart, totalParts);
    }

    // Seek file to resume position
    uint32_t resumeOffset = (startPart - 1) * S3_CHUNK_SIZE;
    if (resumeOffset > 0) {
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        f.seekSet(resumeOffset);
        xSemaphoreGive(g_sd_mutex);
    }

    uint32_t xfrStart = millis();

    // Upload each part
    for (uint32_t partNum = startPart; partNum <= totalParts; partNum++) {
        uint32_t offset = (partNum - 1) * S3_CHUNK_SIZE;
        uint32_t chunkSize = fileSize - offset;
        if (chunkSize > S3_CHUNK_SIZE) chunkSize = S3_CHUNK_SIZE;

        // Get pre-signed URL for this part
        char query[512];
        snprintf(query, sizeof(query), "upload_id=%s&key=%s&part=%u",
                 uploadId, s3Key, partNum);
        String resp = s3ApiGet(query);
        String url = jsonStr(resp, "\"url\"");
        if (!url.length()) {
            DBG.printf("S3: presign part %u failed: %s\n", partNum,
                       resp.substring(0, 200).c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Parse URL (reuses static s3Path buffer from single-PUT path above)
        char s3Host[128];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            DBG.println("S3: URL parse failed");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        DBG.printf("S3: part %u URL len=%d path=%d\n", partNum, url.length(), (int)strlen(s3Path));

        // PUT this chunk
        WiFiClientSecure tls; tls.setInsecure();
        if (!tls.connect(s3Host, 443)) {
            DBG.printf("S3: TLS connect failed to %s (part %u)\n", s3Host, partNum);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        enlargeSendBuffer(tls);

        tls.printf("PUT %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   s3Path, s3Host, chunkSize);

        if (!httpStreamChunk(tls, f, chunkSize)) {
            DBG.printf("S3: stream failed at part %u\n", partNum);
            tls.stop();
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Read response and capture ETag
        char etag[64] = "";
        String putResp = httpReadResponse(tls, etag, sizeof(etag));

        if (!etag[0]) {
            DBG.printf("S3: no ETag for part %u, resp(%d): %s\n",
                       partNum, putResp.length(), putResp.substring(0, 300).c_str());
            s3ClearSession();  // stale upload_id — next retry starts fresh
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Persist progress + etag to NVS
        {
            Preferences p; p.begin("s3up", false);
            char etagKey[12]; snprintf(etagKey, sizeof(etagKey), "etag%u", partNum);
            p.putString(etagKey, etag);
            p.putUInt("part", partNum + 1);
            p.end();
        }

        float elapsed = (millis() - xfrStart) / 1000.0f;
        uint32_t totalSent = offset + chunkSize;
        DBG.printf("S3: part %u/%u done (%u/%u bytes, %.0f%%, %.0f KB/s)\n",
                   partNum, totalParts, totalSent, fileSize,
                   totalSent * 100.0f / fileSize,
                   elapsed > 0 ? totalSent / 1024.0f / elapsed : 0);
    }
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);

    // ── Complete multipart upload ────────────────────────────────────────────
    // Build parts JSON array: [{"part":1,"etag":"..."},...]
    String partsJson = "";
    {
        Preferences p; p.begin("s3up", true);
        for (uint32_t i = 1; i <= totalParts; i++) {
            char etagKey[12]; snprintf(etagKey, sizeof(etagKey), "etag%u", i);
            String etag = p.getString(etagKey, "");
            if (i > 1) partsJson += ",";
            partsJson += "{\"part\":" + String(i) + ",\"etag\":\"" + etag + "\"}";
        }
        p.end();
    }

    if (!s3ApiComplete(uploadId, s3Key, partsJson.c_str())) {
        DBG.println("S3: complete_multipart failed — clearing session");
        s3ClearSession();  // stale upload — next retry starts fresh
        return false;
    }

    s3ClearSession();

    float elapsed = (millis() - xfrStart) / 1000.0f;
    DBG.printf("S3: uploaded '%s' OK (%u bytes, %u parts, %.0f KB/s)\n",
               name, fileSize, totalParts,
               elapsed > 0 ? fileSize / 1024.0f / elapsed : 0);
    return true;
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
                    if (entry.isDir()) { entry.close(); continue; }
                    char tmp[64]; entry.getName(tmp, sizeof(tmp));
                    // Skip .done__ markers and other hidden files
                    if (tmp[0] == '.') { entry.close(); continue; }
                    // Skip 0-byte files (stale cache entries)
                    if (entry.fileSize() == 0) { entry.close(); continue; }
                    strlcpy(name, tmp, sizeof(name));
                    entry.close();
                    break;
                }
                root.close();
            }
            xSemaphoreGive(g_sd_mutex);

            if (!name[0]) { DBG.println("Upload: no files in /harvested/"); break; }

            char path[80];
            snprintf(path, sizeof(path), "/harvested/%s", name);
            DBG.printf("Upload: found %s\n", path);

            // Get file size before upload attempt
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            float fileMb = 0.0f;
            { FsFile f;
              if (f.open(&sd, path, O_RDONLY)) { fileMb = (float)f.fileSize() / 1e6f; f.close(); }
            }
            xSemaphoreGive(g_sd_mutex);

            // Wait for WiFi before attempting FTP (may not be connected yet at boot)
            {
                uint32_t waited = 0;
                while (!g_netConnected && waited < 90000) {
                    if (waited == 0) DBG.println("Upload: waiting for WiFi...");
                    vTaskDelay(pdMS_TO_TICKS(2000)); waited += 2000;
                }
            }
            if (!g_netConnected) {
                DBG.printf("Upload: no WiFi after 90s — will retry in 60s\n");
                vTaskDelay(pdMS_TO_TICKS(60000)); continue;
            }

            // Upload to S3; on failure retry after a delay
            DBG.printf("Uploading: %s (%.1f MB) heap=%u min=%u\n",
                       name, fileMb, ESP.getFreeHeap(), ESP.getMinFreeHeap());
            bool uploaded = s3UploadFile(name);
            if (!uploaded) {
                DBG.printf("Upload failed for %s — retrying in 30s\n", name);
                vTaskDelay(pdMS_TO_TICKS(30000)); continue;  // retry same file
            }

            // Delete the /harvested/ copy and create a .done marker so the
            // next harvest doesn't re-copy the original file.
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            sd.remove(path);
            // Create .done marker: /harvested/.done__<name>
            char donePath[160];
            snprintf(donePath, sizeof(donePath), "/harvested/.done__%s", name);
            { FsFile m; m.open(&sd, donePath, O_WRONLY | O_CREAT); m.close(); }
            xSemaphoreGive(g_sd_mutex);
            DBG.printf("Uploaded & marked done: %s\n", name);
            if (g_filesQueued > 0) g_filesQueued--;
            g_filesUploaded++;
            g_mbUploaded += fileMb;
            if (g_mbQueued >= fileMb) g_mbQueued -= fileMb; else g_mbQueued = 0.0f;
        }
        DBG.printf("Upload idle — %u uploaded\n", g_filesUploaded);
    }
}

// ── Harvest ────────────────────────────────────────────────────────────────────
static void doHarvest() {
    DBG.println("doHarvest: start");
    g_harvesting = true;
    MSC.mediaPresent(false);  // tell host the drive was ejected — clean unmount
    DBG.println("doHarvest: media ejected");
    delay(500);   // give host time to process the media-not-present status
    updateDisplay();

    // Take mutex to exclude uploadTask from the SD bus for the entire harvest.
    // MSC callbacks are already blocked by g_harvesting=true.
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    DBG.println("doHarvest: got mutex, calling sd.begin");

    // sd.begin() re-initializes the card (CMD0/8/41) and re-mounts the FS.
    // After the host's raw MSC writes the card may be in power-save mode;
    // retry up to 3 times with a short delay to give it time to wake up.
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = sd.begin(g_cfg);
        DBG.printf("doHarvest: sd.begin attempt %d = %s\n", i+1, ok ? "OK" : "FAIL");
        if (!ok) delay(500);
    }

    if (!ok) {
        xSemaphoreGive(g_sd_mutex);
        // Reset harvest state so we don't immediately retry in loop()
        g_writeDetected = false; g_lastWriteMs = 0;
        g_hostWasConnected = false; g_hostConnected = false;
        g_harvesting = false;
        MSC.mediaPresent(true);   // re-insert media even on failure
        DBG.println("doHarvest: sd.begin failed, media re-inserted");
        return;
    }

    // Ensure /harvested directory exists
    if (!sd.exists("/harvested")) sd.mkdir("/harvested");

    // Recursively COPY new files to /harvested/ for upload.
    // Originals stay on the USB drive — the host always sees its files.
    // Flattens paths: /logs/data.bin → /harvested/logs__data.bin
    //
    // Skip logic (avoid re-uploading):
    //   - /harvested/<name> exists with same size → pending upload, skip
    //   - /harvested/.done__<name> exists → already uploaded, skip
    uint16_t count = 0; float usedMb = 0.0f;

    // Stack-based directory walk (avoids recursive function calls on 16 KB stack)
    struct DirFrame { FsFile dir; char prefix[80]; };
    DirFrame stack[4];  // max 4 levels deep
    int depth = 0;
    stack[0].dir.open(&sd, "/", O_RDONLY);
    stack[0].prefix[0] = '\0';

    while (depth >= 0) {
        FsFile entry;
        if (!entry.openNext(&stack[depth].dir, O_RDONLY)) {
            stack[depth].dir.close();
            depth--;
            continue;
        }
        char name[64];
        entry.getName(name, sizeof(name));

        if (entry.isHidden() || entry.isSystem() || isSkipped(name)) {
            entry.close(); continue;
        }

        if (entry.isDir()) {
            entry.close();
            if (depth < 3) {
                depth++;
                char subpath[80];
                if (stack[depth-1].prefix[0])
                    snprintf(subpath, sizeof(subpath), "%s/%s", stack[depth-1].prefix, name);
                else
                    snprintf(subpath, sizeof(subpath), "/%s", name);
                stack[depth].dir.open(&sd, subpath, O_RDONLY);
                if (stack[depth-1].prefix[0])
                    snprintf(stack[depth].prefix, sizeof(stack[depth].prefix),
                             "%s__%s", stack[depth-1].prefix, name);
                else
                    strlcpy(stack[depth].prefix, name, sizeof(stack[depth].prefix));
            }
            continue;
        }

        uint32_t fileBytes = entry.fileSize();
        float fileMb = (float)fileBytes / 1e6f;
        entry.close();
        if (fileBytes == 0) continue;  // skip empty files

        // Build destination name (flatten subdirs with __)
        char dstName[128];
        if (stack[depth].prefix[0])
            snprintf(dstName, sizeof(dstName), "%s__%s", stack[depth].prefix, name);
        else
            strlcpy(dstName, name, sizeof(dstName));

        // Skip if already uploaded (.done marker exists)
        char donePath[160];
        snprintf(donePath, sizeof(donePath), "/harvested/.done__%s", dstName);
        if (sd.exists(donePath)) continue;

        // Skip if already pending upload (same-size copy in /harvested/)
        char dst[160];
        snprintf(dst, sizeof(dst), "/harvested/%s", dstName);
        if (sd.exists(dst)) {
            FsFile dup; dup.open(&sd, dst, O_RDONLY);
            if (dup && dup.fileSize() == fileBytes) { dup.close(); continue; }
            if (dup) { dup.close(); sd.remove(dst); }  // stale/wrong size — replace
        }

        // Build source path from prefix
        char src[128];
        if (stack[depth].prefix[0]) {
            char srcDir[80] = "/";
            strlcpy(srcDir + 1, stack[depth].prefix, sizeof(srcDir) - 1);
            for (char* p = srcDir + 1; *p; p++) {
                if (p[0] == '_' && p[1] == '_') { p[0] = '/'; memmove(p+1, p+2, strlen(p+1)); }
            }
            snprintf(src, sizeof(src), "%s/%s", srcDir, name);
        } else {
            snprintf(src, sizeof(src), "/%s", name);
        }

        // Copy file (original stays in place on USB drive)
        FsFile sf, df;
        bool copied = false;
        if (sf.open(&sd, src, O_RDONLY) && df.open(&sd, dst, O_WRONLY | O_CREAT)) {
            static uint8_t cpbuf[8192];  // static: save stack in 16 KB harvest task
            uint32_t rem = fileBytes;
            copied = true;
            while (rem > 0) {
                int n = sf.read(cpbuf, min((uint32_t)sizeof(cpbuf), rem));
                if (n <= 0 || df.write(cpbuf, n) != n) { copied = false; break; }
                rem -= n;
            }
            sf.close(); df.close();
            if (!copied) sd.remove(dst);  // clean up partial copy
        }
        if (copied) {
            DBG.printf("Harvested: %s → %s (%.1f MB)\n", src, dstName, fileMb);
            usedMb += fileMb; count++;
        } else {
            DBG.printf("Harvest copy failed: %s\n", src);
        }
    }

    // Flush SdFat's dirty cache to the physical SD card.  MSC reads raw
    // sectors (bypassing cache), so without this the host sees stale data.
    // sd.end()/sd.begin() is too heavy (breaks MSC).  Instead, force the
    // last dirty cache sector out by reading a sector that isn't cached.
    if (count > 0) {
        uint8_t dummy[512];
        sd.card()->readSector(0, dummy);  // evicts dirty cache sector
        DBG.println("doHarvest: cache flushed");
    }

    xSemaphoreGive(g_sd_mutex);

    g_filesQueued += count;
    if (count > 0) g_mbQueued += usedMb;
    DBG.printf("doHarvest: done %u file(s) (%.1f MB)\n", count, usedMb);

    g_writeDetected = false; g_lastWriteMs = 0;
    g_hostWasConnected = false; g_hostConnected = false;
    g_hostWrittenMb = 0.0f;

    g_harvesting = false;
    MSC.mediaPresent(true);
    g_lastHarvestMs = millis();
    // Adaptive cooldown: short after productive harvest, long after empty
    // (empty harvest → host metadata re-triggers → need longer cooldown)
    g_harvestCoolMs = (count > 0) ? QUIET_WINDOW_MS : 300000UL;
    DBG.printf("doHarvest: media re-inserted (%u files, cooldown %us)\n",
               count, g_harvestCoolMs / 1000);

    if (count > 0 && g_upload_task) xTaskNotifyGive(g_upload_task);
}

static void harvestTask(void* /*param*/) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for signal from loop()
        doHarvest();
    }
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

    // Crash-loop detection: count rapid reboots via NVS.
    // If >5 reboots within seconds of each other, pause to allow debugging.
    {
        Preferences bc; bc.begin("dbg", false);
        uint32_t boots = bc.getUInt("boots", 0) + 1;
        bc.putUInt("boots", boots);
        bc.end();
        esp_reset_reason_t reason = esp_reset_reason();
        DBG.printf("Boot #%u  reset_reason=%d  heap=%u\n",
                   boots, (int)reason, ESP.getFreeHeap());
        if (boots > 5 && reason != ESP_RST_POWERON) {
            DBG.println("CRASH LOOP DETECTED — pausing 30s for debug");
            disp("CRASH LOOP", "Paused 30s");
            delay(30000);
            // Reset counter so next boot proceeds normally
            Preferences bc2; bc2.begin("dbg", false);
            bc2.putUInt("boots", 0);
            bc2.end();
        }
    }

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        for (;;) { DBG.println("SSD1306 failed"); delay(1000); }
    }
    // Provision S3 upload credentials on first boot (if NVS is empty)
    {
        Preferences p; p.begin("s3", false);
        if (p.getString("api_host", "").isEmpty()) {
            p.putString("api_host", "disw6oxjed.execute-api.us-west-2.amazonaws.com");
            p.putString("api_key",  "7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm");
            DBG.println("S3 upload credentials provisioned");
        }
        // Auto-generate device_id from MAC address if not set
        if (p.getString("device_id", "").isEmpty()) {
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            char macStr[16];
            snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            p.putString("device_id", macStr);
            DBG.printf("Device ID: %s\n", macStr);
        }
        p.end();
    }

    disp("Init SD...");

    spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    // sd.begin(): full init — card init (CMD0/8/41) + FAT volume mount.
    // NEVER auto-format: a corrupt FAT after power loss is recoverable,
    // but formatting destroys all data.  Only format on explicit CLI command.
    {
        bool has_fs = false;
        for (int i = 1; i <= 10 && !has_fs; i++) {
            has_fs = sd.begin(g_cfg);
            if (!has_fs) {
                DBG.printf("SD init attempt %d failed\n", i);
                disp("SD init...", i <= 5 ? "retrying" : "check card");
                delay(1000);
            }
        }
        if (!has_fs) {
            DBG.println("SD init failed — NOT formatting (data preservation)");
            DBG.println("Use CLI command FORMAT to format if card is truly blank");
            disp("SD FAILED", "serial: FORMAT");
            // Continue without SD — USB MSC won't work but WiFi/CLI still function
            // so the user can issue FORMAT via serial if the card is genuinely empty.
        }
        if (has_fs) {
            g_card_sectors = sd.card()->sectorCount();
            DBG.printf("SD OK: %lu sectors (%.0f MB)\n",
                       g_card_sectors, g_card_sectors * 512.0f / 1e6f);
        }
    }

    g_sdTotalMb = g_card_sectors * 512.0f / 1e6f;

    if (g_card_sectors > 0) {
        // MSC: raw sector access through sd.card() bypasses sd's FS cache.
        // sd.begin() in doHarvest() refreshes the cache each harvest cycle.
        MSC.begin(g_card_sectors, 512);
        MSC.mediaPresent(true);
        g_sd_ready = true;
        DBG.printf("MSC ready: %lu sectors (%.0f MB)\n", g_card_sectors, g_sdTotalMb);
        disp("USB drive ready", "");
    } else {
        disp("No SD card", "CLI: FORMAT");
    }
    g_lastDisplayMs = millis();

    xTaskCreatePinnedToCore(uploadTask,  "upload",  16384, nullptr, 1, &g_upload_task,  1);
    xTaskCreatePinnedToCore(harvestTask, "harvest", 16384, nullptr, 1, &g_harvest_task, 1);
    xTaskCreatePinnedToCore(wifiTask,    "wifi",    8192, nullptr, 1, &g_wifi_task,    1);

    // Scan /harvested/ for files left over from before the last reboot.
    // The upload task starts blocked; notify it if there is work to do.
    if (g_sd_ready) {
        FsFile h, f;
        if (h.open(&sd, "/harvested", O_RDONLY)) {
            while (f.openNext(&h, O_RDONLY)) {
                char name[64]; f.getName(name, sizeof(name));
                if (!f.isDir() && name[0] != '.' && f.fileSize() > 0) {
                    g_filesQueued++;
                    g_mbQueued += (float)f.fileSize() / 1e6f;
                }
                f.close();
            }
            h.close();
            if (g_filesQueued > 0) {
                DBG.printf("Boot: found %u file(s) in /harvested/ — notifying upload\n", g_filesQueued);
                xTaskNotifyGive(g_upload_task);
            }
        }
    }

    DBG.printf("setup done — free heap: %u, min: %u\n",
               ESP.getFreeHeap(), ESP.getMinFreeHeap());
}

// ── Serial CLI — credential provisioning for automated testing ─────────────────
// Commands (newline-terminated):
//   SETWIFI <ssid> <pass>
//   SETFTP  <host> <port> <user> <pass> <remote_path>
//   STATUS
//   REBOOT
static void processCLI(const char* cmd) {
    if (strncmp(cmd, "SETWIFI ", 8) == 0) {
        const char* rest = cmd + 8;
        const char* sp   = strchr(rest, ' ');
        char ssid[33] = "", pass[65] = "";
        if (sp) {
            int slen = sp - rest;
            strlcpy(ssid, rest, min(slen + 1, (int)sizeof(ssid)));
            strlcpy(pass, sp + 1, sizeof(pass));
        } else {
            strlcpy(ssid, rest, sizeof(ssid));
        }
        if (!ssid[0]) { DBG.println("CLI: SETWIFI <ssid> <pass>"); return; }
        saveNetwork(ssid, pass);
        DBG.printf("CLI: WiFi saved: '%s'\n", ssid);

    } else if (strncmp(cmd, "SETS3 ", 6) == 0) {
        char apiHost[128], apiKey[64];
        if (sscanf(cmd + 6, "%127s %63s", apiHost, apiKey) < 2) {
            DBG.println("CLI: SETS3 <api_host> <api_key>"); return;
        }
        Preferences p; p.begin("s3", false);
        p.putString("api_host", apiHost);
        p.putString("api_key", apiKey);
        p.end();
        g_apiHost[0] = 0; g_apiKey[0] = 0;  // force reload on next upload
        DBG.printf("CLI: S3 saved: api_host=%s\n", apiHost);

    } else if (strcmp(cmd, "STATUS") == 0) {
        char ipStr[20] = "disconnected";
        if (g_netConnected) strlcpy(ipStr, WiFi.localIP().toString().c_str(), sizeof(ipStr));
        DBG.printf("CLI: wifi=%s ap=%s files_q=%u files_up=%u mbq=%.2f mbup=%.2f\n",
            ipStr, g_apMode ? "yes" : "no",
            g_filesQueued, g_filesUploaded,
            g_mbQueued, g_mbUploaded);
        Preferences p; p.begin("s3", true);
        DBG.printf("CLI: s3 api_host=%s device=%s\n",
            p.getString("api_host","(none)").c_str(),
            p.getString("device_id","(none)").c_str());
        p.end();

    } else if (strcmp(cmd, "RESETBOOT") == 0) {
        Preferences bc; bc.begin("dbg", false);
        bc.putUInt("boots", 0);
        bc.end();
        DBG.println("CLI: boot counter reset");

    } else if (strcmp(cmd, "UPLOAD") == 0) {
        if (g_upload_task) {
            DBG.println("CLI: triggering upload task");
            xTaskNotifyGive(g_upload_task);
        }

    } else if (strcmp(cmd, "FORMAT") == 0) {
        DBG.println("CLI: formatting SD card — ALL DATA WILL BE LOST");
        g_harvesting = true;  // block MSC
        MSC.mediaPresent(false);
        delay(500);
        if (sd.format(&DBG)) {
            DBG.println("CLI: format OK");
            sd.begin(g_cfg);
            g_card_sectors = sd.card()->sectorCount();
            g_sd_ready = true;
        } else {
            DBG.println("CLI: format FAILED");
        }
        g_harvesting = false;
        MSC.mediaPresent(true);

    } else if (strcmp(cmd, "REBOOT") == 0) {
        DBG.println("CLI: rebooting...");
        delay(200);
        ESP.restart();

    } else {
        DBG.printf("CLI: unknown: '%s'\n", cmd);
        DBG.println("CLI: commands: SETWIFI SETS3 STATUS UPLOAD FORMAT REBOOT");
    }
}

// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    // Print buffered harvest log after serial reconnects (USB may have reset during harvest)
    if (g_harvest_log[0]) {
        DBG.print(g_harvest_log);
        g_harvest_log[0] = '\0';
    }

    // Serial CLI: accumulate characters into a line, process on newline
    while (DBG.available()) {
        static char cliBuf[128];
        static int  cliLen = 0;
        char c = DBG.read();
        if (c == '\n' || c == '\r') {
            if (cliLen > 0) { cliBuf[cliLen] = 0; processCLI(cliBuf); cliLen = 0; }
        } else if (cliLen < (int)sizeof(cliBuf) - 1) {
            cliBuf[cliLen++] = c;
        }
    }

    uint32_t now = millis();
    if (g_sd_ready && now - g_lastDisplayMs >= DISPLAY_INTERVAL_MS) {
        g_lastDisplayMs = now;
        updateDisplay();
    }
    if (!g_harvesting && g_writeDetected && g_hostWasConnected &&
        g_lastWriteMs != 0 && (now - g_lastWriteMs) >= QUIET_WINDOW_MS &&
        (now - g_lastHarvestMs) >= g_harvestCoolMs &&  // adaptive cooldown
        g_hostWrittenMb > 0.01f) {  // >10 KB — ignore metadata-only writes (deletes, mounts)
        DBG.printf("Harvest trigger: %.1f KB written, %us idle\n",
                   g_hostWrittenMb * 1024.0f, (now - g_lastWriteMs) / 1000);
        if (g_harvest_task) xTaskNotifyGive(g_harvest_task);
    }
    delay(100);
}

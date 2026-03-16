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
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include "mbedtls/base64.h"

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

// ── Dropbox upload ─────────────────────────────────────────────────────────────
// Uses OAuth2 refresh-token flow: the refresh token (stored in NVS) never
// expires and mints short-lived access tokens on demand.
//
// NVS namespace "dbx": app_key, app_secret, refresh_token
// Cached access token lives in RAM — refreshed when expired or on 401.

static char   g_dbxToken[2048] = "";   // current access token (short-lived)
static uint32_t g_dbxTokenExpMs = 0;   // millis() when token expires

// Mint a fresh access token using the refresh token.
static bool dbxRefreshToken() {
    Preferences p; p.begin("dbx", true);
    String appKey    = p.getString("app_key", "");
    String appSecret = p.getString("app_secret", "");
    String refresh   = p.getString("refresh", "");
    p.end();

    if (!appKey.length() || !refresh.length()) {
        DBG.println("DBX: no credentials in NVS");
        return false;
    }

    WiFiClientSecure tls;
    tls.setInsecure();  // skip cert verification (saves ~40 KB RAM)
    if (!tls.connect("api.dropboxapi.com", 443)) {
        DBG.println("DBX: TLS connect failed (token)");
        return false;
    }

    // POST body
    String body = "grant_type=refresh_token&refresh_token=" + refresh;

    // Basic auth header: base64(app_key:app_secret)
    String cred = appKey + ":" + appSecret;
    // Simple base64 encode (ESP32 Arduino has no built-in b64 for strings)
    size_t outLen = 0;
    mbedtls_base64_encode(nullptr, 0, &outLen,
        (const uint8_t*)cred.c_str(), cred.length());
    char* b64 = (char*)malloc(outLen + 1);
    mbedtls_base64_encode((uint8_t*)b64, outLen, &outLen,
        (const uint8_t*)cred.c_str(), cred.length());
    b64[outLen] = 0;

    tls.printf("POST /oauth2/token HTTP/1.1\r\n"
               "Host: api.dropboxapi.com\r\n"
               "Authorization: Basic %s\r\n"
               "Content-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: %d\r\n"
               "Connection: close\r\n\r\n",
               b64, body.length());
    tls.print(body);
    free(b64);

    // Read response — find access_token in JSON body
    // Skip HTTP headers
    String statusLine = tls.readStringUntil('\n');
    DBG.printf("DBX: HTTP %s\n", statusLine.c_str());
    while (tls.connected()) {
        String line = tls.readStringUntil('\n');
        if (line == "\r") break;
    }
    // Read body (may arrive in chunks; wait up to 5s for data)
    String resp = "";
    uint32_t t1 = millis();
    while (millis() - t1 < 5000) {
        if (tls.available()) {
            resp += tls.readString();
            t1 = millis();  // reset timer on data
        } else if (!tls.connected()) {
            break;
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    tls.stop();
    DBG.printf("DBX: resp (%d bytes): %s\n", resp.length(),
               resp.substring(0, 300).c_str());

    // Extract access_token from JSON (avoid pulling in ArduinoJson)
    int pos = resp.indexOf("\"access_token\"");
    if (pos < 0) {
        DBG.println("DBX: token refresh failed — no access_token in response");
        return false;
    }
    // Find the value: skip to the colon, then the opening quote
    int q1 = resp.indexOf('"', resp.indexOf(':', pos) + 1);
    int q2 = resp.indexOf('"', q1 + 1);
    int tokenLen = (q1 >= 0 && q2 >= 0) ? q2 - q1 - 1 : -1;
    if (q1 < 0 || q2 < 0 || tokenLen > (int)sizeof(g_dbxToken) - 1) {
        DBG.printf("DBX: token parse failed q1=%d q2=%d len=%d max=%d\n",
                   q1, q2, tokenLen, (int)sizeof(g_dbxToken) - 1);
        return false;
    }
    resp.substring(q1 + 1, q2).toCharArray(g_dbxToken, sizeof(g_dbxToken));

    // Extract expires_in
    int epos = resp.indexOf("\"expires_in\"");
    uint32_t expiresIn = 14400;
    if (epos >= 0) {
        int colon = resp.indexOf(':', epos);
        expiresIn = (uint32_t)resp.substring(colon + 1).toInt();
    }
    g_dbxTokenExpMs = millis() + (expiresIn - 300) * 1000UL;  // refresh 5 min early

    DBG.printf("DBX: token refreshed, expires in %us\n", expiresIn);
    return true;
}

// ── Dropbox chunked upload with resume ────────────────────────────────────────
// Uses upload_session/start → append_v2 → finish so uploads survive power loss.
// Session state (session_id + offset) is persisted to NVS after each chunk.
// On boot, if a session is in progress, we resume from the stored offset.
// Dropbox sessions last 7 days.
//
// Chunk size per HTTPS request.  Larger = fewer TLS handshakes = faster,
// but more data lost on power-cut.  4 MB is a good balance.
#define DBX_CHUNK_SIZE (4UL * 1024 * 1024)

// Read HTTP response body, skipping headers.  Returns body as String.
// Handles chunked transfer encoding by stripping chunk-size lines.
static String dbxReadResponse(WiFiClientSecure& tls) {
    bool chunked = false;
    while (tls.connected()) {
        String line = tls.readStringUntil('\n');
        if (line.indexOf("chunked") >= 0) chunked = true;
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
        // Parse chunk size (hex)
        String szLine = raw.substring(pos, nl);
        szLine.trim();
        unsigned long chunkSz = strtoul(szLine.c_str(), nullptr, 16);
        if (chunkSz == 0) break;  // final chunk
        int dataStart = nl + 1;
        body += raw.substring(dataStart, dataStart + (int)chunkSz);
        pos = dataStart + (int)chunkSz + 2;  // skip data + \r\n
    }
    return body;
}

// Stream `len` bytes from an open FsFile at current position to a TLS connection.
// Returns true if all bytes sent.
static bool dbxStreamChunk(WiFiClientSecure& tls, FsFile& f, uint32_t len) {
    uint8_t cbuf[8192];
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
static void dbxClearSession() {
    Preferences p; p.begin("dbxup", false);
    p.clear();
    p.end();
}

// Upload a file from /harvested/<name> to Dropbox using resumable sessions.
static bool dbxUploadFile(const char* name) {
    if (!g_netConnected) { DBG.println("DBX: no WiFi"); return false; }
    if (!g_dbxToken[0] || millis() >= g_dbxTokenExpMs) {
        if (!dbxRefreshToken()) return false;
    }

    // Open file
    char fpath[80]; snprintf(fpath, sizeof(fpath), "/harvested/%s", name);
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FsFile f; bool fok = f.open(&sd, fpath, O_RDONLY);
    uint32_t fileSize = fok ? f.fileSize() : 0;
    xSemaphoreGive(g_sd_mutex);
    if (!fok || fileSize == 0) { DBG.printf("DBX: can't open %s\n", fpath); return false; }

    // Check for a stored session to resume
    char sessionId[128] = "";
    uint32_t offset = 0;
    {
        Preferences p; p.begin("dbxup", true);
        String storedName = p.getString("name", "");
        if (storedName == name) {
            strlcpy(sessionId, p.getString("sid", "").c_str(), sizeof(sessionId));
            offset = p.getUInt("offset", 0);
        }
        p.end();
    }

    // ── Start new session if none stored ──────────────────────────────────────
    if (!sessionId[0]) {
        offset = 0;
        WiFiClientSecure tls; tls.setInsecure();
        if (!tls.connect("content.dropboxapi.com", 443)) {
            DBG.println("DBX: TLS connect failed (start)");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Start with 0 bytes — just get a session ID
        tls.printf("POST /2/files/upload_session/start HTTP/1.1\r\n"
                   "Host: content.dropboxapi.com\r\n"
                   "Authorization: Bearer %s\r\n"
                   "Dropbox-API-Arg: {}\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: 0\r\n"
                   "Connection: close\r\n\r\n", g_dbxToken);

        String resp = dbxReadResponse(tls);
        // Parse session_id
        int q1 = resp.indexOf("\"session_id\"");
        if (q1 < 0) {
            DBG.printf("DBX: start failed: %s\n", resp.substring(0, 200).c_str());
            if (resp.indexOf("invalid_access_token") >= 0) g_dbxToken[0] = 0;
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        int v1 = resp.indexOf('"', resp.indexOf(':', q1) + 1);
        int v2 = resp.indexOf('"', v1 + 1);
        resp.substring(v1 + 1, v2).toCharArray(sessionId, sizeof(sessionId));
        DBG.printf("DBX: session started (%d chars): %s\n",
                   (int)strlen(sessionId), sessionId);

        // Persist session immediately
        Preferences p; p.begin("dbxup", false);
        p.putString("sid", sessionId);
        p.putString("name", name);
        p.putUInt("offset", 0);
        p.putUInt("size", fileSize);
        p.end();
    } else {
        DBG.printf("DBX: resuming session %s at offset %u/%u\n",
                   sessionId, offset, fileSize);
    }

    // Seek file to resume offset
    if (offset > 0) {
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        f.seekSet(offset);
        xSemaphoreGive(g_sd_mutex);
    }

    // ── Send chunks via append_v2 ────────────────────────────────────────────
    uint32_t xfrStart = millis();
    while (offset < fileSize) {
        if (!g_dbxToken[0] || millis() >= g_dbxTokenExpMs) {
            if (!dbxRefreshToken()) {
                xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
                return false;
            }
        }

        uint32_t chunkSize = fileSize - offset;
        if (chunkSize > DBX_CHUNK_SIZE) chunkSize = DBX_CHUNK_SIZE;

        WiFiClientSecure tls; tls.setInsecure();
        if (!tls.connect("content.dropboxapi.com", 443)) {
            DBG.println("DBX: TLS connect failed (append)");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        char apiArg[256];
        snprintf(apiArg, sizeof(apiArg),
            "{\"cursor\":{\"session_id\":\"%s\",\"offset\":%u},\"close\":false}",
            sessionId, offset);

        tls.printf("POST /2/files/upload_session/append_v2 HTTP/1.1\r\n"
                   "Host: content.dropboxapi.com\r\n"
                   "Authorization: Bearer %s\r\n"
                   "Dropbox-API-Arg: %s\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %u\r\n"
                   "Connection: close\r\n\r\n",
                   g_dbxToken, apiArg, chunkSize);

        if (!dbxStreamChunk(tls, f, chunkSize)) {
            DBG.printf("DBX: stream failed at offset %u\n", offset);
            tls.stop();
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Check response (append returns empty body on success, or error JSON)
        String statusLine = tls.readStringUntil('\n');
        String resp = dbxReadResponse(tls);

        if (statusLine.indexOf("200") < 0) {
            DBG.printf("DBX: append failed at %u: %s %s\n", offset,
                       statusLine.c_str(), resp.substring(0, 200).c_str());
            if (resp.indexOf("invalid_access_token") >= 0) g_dbxToken[0] = 0;
            if (resp.indexOf("not_found") >= 0) dbxClearSession();  // stale session
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        offset += chunkSize;

        // Persist progress to NVS after each successful chunk
        {
            Preferences p; p.begin("dbxup", false);
            p.putUInt("offset", offset);
            p.end();
        }

        float elapsed = (millis() - xfrStart) / 1000.0f;
        DBG.printf("DBX: %u/%u bytes (%.0f%%, %.0f KB/s)\n",
                   offset, fileSize, offset * 100.0f / fileSize,
                   elapsed > 0 ? offset / 1024.0f / elapsed : 0);
    }
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY); f.close(); xSemaphoreGive(g_sd_mutex);

    // ── Finish: commit the file ──────────────────────────────────────────────
    {
        if (!g_dbxToken[0] || millis() >= g_dbxTokenExpMs) {
            if (!dbxRefreshToken()) return false;
        }

        WiFiClientSecure tls; tls.setInsecure();
        if (!tls.connect("content.dropboxapi.com", 443)) {
            DBG.println("DBX: TLS connect failed (finish)");
            return false;
        }

        char apiArg[256];
        snprintf(apiArg, sizeof(apiArg),
            "{\"cursor\":{\"session_id\":\"%s\",\"offset\":%u},"
            "\"commit\":{\"path\":\"/%s\",\"mode\":\"add\",\"autorename\":true}}",
            sessionId, fileSize, name);

        tls.printf("POST /2/files/upload_session/finish HTTP/1.1\r\n"
                   "Host: content.dropboxapi.com\r\n"
                   "Authorization: Bearer %s\r\n"
                   "Dropbox-API-Arg: %s\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: 0\r\n"
                   "Connection: close\r\n\r\n",
                   g_dbxToken, apiArg);

        String resp = dbxReadResponse(tls);
        if (resp.indexOf("\"path_display\"") < 0) {
            DBG.printf("DBX: finish failed: %s\n", resp.substring(0, 200).c_str());
            return false;
        }
    }

    // Success — clear stored session
    dbxClearSession();

    float elapsed = (millis() - xfrStart) / 1000.0f;
    DBG.printf("DBX: uploaded '%s' OK (%u bytes, %.0f KB/s)\n", name, fileSize,
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
                    if (!entry.isDir()) { entry.getName(name, sizeof(name)); entry.close(); break; }
                    entry.close();
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
            { FsFile f; if (f.open(&sd, path, O_RDONLY)) { fileMb = (float)f.fileSize() / 1e6f; f.close(); } }
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

            // Upload to Dropbox; on failure retry after a delay
            DBG.printf("Uploading: %s (%.1f MB) heap=%u min=%u\n",
                       name, fileMb, ESP.getFreeHeap(), ESP.getMinFreeHeap());
            bool uploaded = dbxUploadFile(name);
            if (!uploaded) {
                DBG.printf("Upload failed for %s — retrying in 30s\n", name);
                vTaskDelay(pdMS_TO_TICKS(30000)); continue;  // retry same file
            }

            // Delete uploaded file.  SdFat's cache may be stale (MSC raw I/O
            // bypasses it), so removal can fail.  If it does, the file stays
            // and will be re-uploaded next cycle — wasteful but not fatal.
            // The next doHarvest() calls sd.begin() which refreshes the cache,
            // so stale entries get cleaned up naturally.
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            bool removed = sd.remove(path);
            xSemaphoreGive(g_sd_mutex);
            if (removed) {
                DBG.printf("Removed %s\n", path);
                if (g_filesQueued > 0) g_filesQueued--;
                g_filesUploaded++;
                g_mbUploaded += fileMb;
                if (g_mbQueued >= fileMb) g_mbQueued -= fileMb; else g_mbQueued = 0.0f;
            } else {
                DBG.printf("sd.remove(%s) failed — will retry next cycle\n", path);
                // Break out so we don't re-upload the same file immediately.
                // Next harvest's sd.begin() will refresh the cache.
                break;
            }
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

    // Note: SdFat writes dirty cache sectors to SD on eviction.
    // MSC reads raw sectors (bypassing cache), so the host sees
    // actual disk state.  No explicit flush needed here.

    xSemaphoreGive(g_sd_mutex);

    g_filesQueued += count;
    if (count > 0) g_mbQueued += usedMb;
    DBG.printf("doHarvest: done %u file(s) (%.1f MB)\n", count, usedMb);

    g_writeDetected = false; g_lastWriteMs = 0;
    g_hostWasConnected = false; g_hostConnected = false;
    g_hostWrittenMb = 0.0f;
    g_harvesting = false;
    MSC.mediaPresent(true);   // re-insert drive — host remounts with fresh data
    DBG.println("doHarvest: media re-inserted");

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
    // Provision Dropbox credentials on first boot (if NVS is empty)
    {
        Preferences p; p.begin("dbx", false);
        if (p.getString("app_key", "").isEmpty()) {
            p.putString("app_key",    "82un1zz0uurgszt");
            p.putString("app_secret", "hc3wapn8hsgzuva");
            p.putString("refresh",    "J0Iqey6GFFsAAAAAAAAAAa7xToJsNmRAqr1Ok5WOGyqGlIhJI0wcSdL2_LKv6quE");
            DBG.println("Dropbox credentials provisioned");
        }
        p.end();
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

    xTaskCreatePinnedToCore(uploadTask,  "upload",  16384, nullptr, 1, &g_upload_task,  1);
    xTaskCreatePinnedToCore(harvestTask, "harvest", 16384, nullptr, 1, &g_harvest_task, 1);
    xTaskCreatePinnedToCore(wifiTask,    "wifi",    8192, nullptr, 1, &g_wifi_task,    1);

    // Scan /harvested/ for files left over from before the last reboot.
    // The upload task starts blocked; notify it if there is work to do.
    {
        FsFile h, f;
        if (h.open(&sd, "/harvested", O_RDONLY)) {
            while (f.openNext(&h, O_RDONLY)) {
                char name[64]; f.getName(name, sizeof(name));
                if (!f.isDir() && !f.isHidden() && !f.isSystem()) {
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

    } else if (strncmp(cmd, "SETDBX ", 7) == 0) {
        char appKey[32], appSecret[32], refresh[128];
        if (sscanf(cmd + 7, "%31s %31s %127s", appKey, appSecret, refresh) < 3) {
            DBG.println("CLI: SETDBX <app_key> <app_secret> <refresh_token>"); return;
        }
        Preferences p; p.begin("dbx", false);
        p.putString("app_key", appKey);
        p.putString("app_secret", appSecret);
        p.putString("refresh", refresh);
        p.end();
        g_dbxToken[0] = 0;  // force re-auth on next upload
        DBG.printf("CLI: Dropbox saved: app_key=%s\n", appKey);

    } else if (strcmp(cmd, "STATUS") == 0) {
        char ipStr[20] = "disconnected";
        if (g_netConnected) strlcpy(ipStr, WiFi.localIP().toString().c_str(), sizeof(ipStr));
        DBG.printf("CLI: wifi=%s ap=%s files_q=%u files_up=%u mbq=%.2f mbup=%.2f\n",
            ipStr, g_apMode ? "yes" : "no",
            g_filesQueued, g_filesUploaded,
            g_mbQueued, g_mbUploaded);
        Preferences p; p.begin("dbx", true);
        DBG.printf("CLI: dropbox app_key=%s refresh=%s\n",
            p.getString("app_key","(none)").c_str(),
            p.getString("refresh","").length() > 0 ? "yes" : "no");
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

    } else if (strcmp(cmd, "REBOOT") == 0) {
        DBG.println("CLI: rebooting...");
        delay(200);
        ESP.restart();

    } else {
        DBG.printf("CLI: unknown: '%s'\n", cmd);
        DBG.println("CLI: commands: SETWIFI SETFTP STATUS REBOOT");
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
    if (now - g_lastDisplayMs >= DISPLAY_INTERVAL_MS) {
        g_lastDisplayMs = now;
        updateDisplay();
    }
    if (!g_harvesting && g_writeDetected && g_hostWasConnected &&
        g_lastWriteMs != 0 && (now - g_lastWriteMs) >= QUIET_WINDOW_MS) {
        DBG.println("Quiet window elapsed — triggering harvest");
        if (g_harvest_task) xTaskNotifyGive(g_harvest_task);
    }
    delay(100);
}

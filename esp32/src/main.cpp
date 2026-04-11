// AirBridge ESP32-S3 Firmware — ESP-IDF native port
// USB Mass Storage + SD card harvest + S3 upload + WiFi + captive portal + OLED
//
// Build: cd esp32 && ~/.local/bin/pio run
// Flash: 1200-baud touch on CDC port, then pio run -t upload

#define FW_VERSION "10.2001.7"

#include <cstring>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
#include <dirent.h>
#include <sys/stat.h>

#include "airbridge_utils.h"
#include "airbridge_proto.h"
#include "hal/hal.h"
#include "airbridge_display.h"
#include "airbridge_wifi_creds.h"
#include "airbridge_harvest.h"
#include "airbridge_http.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "ff.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
#include "tusb_msc_storage.h"
#include "tusb_cdc_acm.h"
#include "class/msc/msc_device.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "dhcpserver/dhcpserver.h"
#include "driver/uart.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "esp_netif_net_stack.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

static const char *TAG = "airbridge";

HAL* g_hal = nullptr;

// ── MSC-only mode (no CDC) ──────────────────────────────────────────────────
// Default: MSC-only for avionics compatibility. Set via CLI: SETMODE CDC / SETMODE MSC
// Persists in NVS. CDC mode gives serial console for debugging + config.
static bool g_msc_only = false;  // default CDC+MSC until SETMODE MSC is run

// ── Utility: millis() equivalent ─────────────────────────────────────────────
static inline uint32_t millis() {
    if (g_hal && g_hal->clock) return g_hal->clock->millis();
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ── Pin assignments ──────────────────────────────────────────────────────────
#define PIN_I2C_SCL   7
#define PIN_I2C_SDA   8
#define PIN_SD_CS    10
#define PIN_SD_MOSI  11
#define PIN_SD_MISO  12
#define PIN_SD_SCK   13

// ── Modem (SIM7600) pin assignments ──────────────────────────────────────
#define PIN_MODEM_TX   43
#define PIN_MODEM_RX   44
#define PIN_MODEM_RTS   1
#define PIN_MODEM_CTS   2

// ── Display constants ────────────────────────────────────────────────────────
// SCREEN_W, SCREEN_H defined in hal/display.h
#define OLED_ADDR  0x3C

// ── SSD1306 OLED driver (ESP32 HAL implementation) ──────────────────────────
class Esp32Display : public IDisplay {
public:
    bool init() override {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = (gpio_num_t)PIN_I2C_SDA;
        bus_cfg.scl_io_num = (gpio_num_t)PIN_I2C_SCL;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        if (i2c_new_master_bus(&bus_cfg, &bus_) != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus init failed");
            return false;
        }

        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = OLED_ADDR;
        dev_cfg.scl_speed_hz = 400000;

        if (i2c_master_bus_add_device(bus_, &dev_cfg, &dev_) != ESP_OK) {
            ESP_LOGE(TAG, "OLED device add failed");
            return false;
        }

        static const uint8_t init_cmds[] = {
            0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
            0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
            0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF,
        };
        for (size_t i = 0; i < sizeof(init_cmds); i++) cmd(init_cmds[i]);
        clear();
        ok_ = true;
        return true;
    }

    void flush() override {
        if (!ok_) return;
        cmd(0x21); cmd(0); cmd(127);
        cmd(0x22); cmd(0); cmd(7);
        for (int page = 0; page < 8; page++) {
            uint8_t buf[SCREEN_W + 1];
            buf[0] = 0x40;
            memcpy(buf + 1, &framebuf[page * SCREEN_W], SCREEN_W);
            i2c_master_transmit(dev_, buf, SCREEN_W + 1, 100);
        }
    }

    bool ok() const override { return ok_; }

private:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t dev_ = nullptr;
    bool ok_ = false;

    void cmd(uint8_t c) {
        uint8_t buf[2] = {0x00, c};
        i2c_master_transmit(dev_, buf, 2, 100);
    }
};

class Esp32Clock : public IClock {
public:
    uint32_t millis() override {
        return (uint32_t)(esp_timer_get_time() / 1000ULL);
    }
    void delay_ms(uint32_t ms) override {
        vTaskDelay(pdMS_TO_TICKS(ms));
    }
};

class Esp32Nvs : public INvs {
public:
    bool get_str(const char* ns, const char* key, char* out, size_t sz) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) { out[0] = '\0'; return false; }
        size_t len = sz;
        bool ok = (nvs_get_str(h, key, out, &len) == ESP_OK);
        if (!ok) out[0] = '\0';
        nvs_close(h);
        return ok;
    }
    bool set_str(const char* ns, const char* key, const char* val) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
        bool ok = (nvs_set_str(h, key, val) == ESP_OK);
        nvs_commit(h); nvs_close(h);
        return ok;
    }
    bool get_u8(const char* ns, const char* key, uint8_t* out) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
        bool ok = (nvs_get_u8(h, key, out) == ESP_OK);
        nvs_close(h);
        return ok;
    }
    bool set_u8(const char* ns, const char* key, uint8_t val) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
        nvs_set_u8(h, key, val); nvs_commit(h); nvs_close(h);
        return true;
    }
    bool get_i32(const char* ns, const char* key, int32_t* out) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
        bool ok = (nvs_get_i32(h, key, out) == ESP_OK);
        nvs_close(h);
        return ok;
    }
    bool set_i32(const char* ns, const char* key, int32_t val) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
        nvs_set_i32(h, key, val); nvs_commit(h); nvs_close(h);
        return true;
    }
    bool get_u32(const char* ns, const char* key, uint32_t* out) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return false;
        bool ok = (nvs_get_u32(h, key, out) == ESP_OK);
        nvs_close(h);
        return ok;
    }
    bool set_u32(const char* ns, const char* key, uint32_t val) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return false;
        nvs_set_u32(h, key, val); nvs_commit(h); nvs_close(h);
        return true;
    }
    void erase_key(const char* ns, const char* key) override {
        nvs_handle_t h;
        if (nvs_open(ns, NVS_READWRITE, &h) != ESP_OK) return;
        nvs_erase_key(h, key); nvs_commit(h); nvs_close(h);
    }
};

class Esp32Filesys : public IFilesys {
public:
    void* open(const char* path, const char* mode) override { return (void*)fopen(path, mode); }
    size_t read(void* f, void* buf, size_t len) override { return fread(buf, 1, len, (FILE*)f); }
    size_t write(void* f, const void* buf, size_t len) override { return fwrite(buf, 1, len, (FILE*)f); }
    bool seek(void* f, long offset, int whence) override { return fseek((FILE*)f, offset, whence) == 0; }
    long tell(void* f) override { return ftell((FILE*)f); }
    void close(void* f) override { fclose((FILE*)f); }
    void* opendir(const char* path) override { return (void*)::opendir(path); }
    bool readdir(void* d, FsDirEntry* entry) override {
        struct dirent* ent = ::readdir((DIR*)d);
        if (!ent) return false;
        strlcpy(entry->name, ent->d_name, sizeof(entry->name));
        // Need stat for size and type
        return true;  // caller uses stat() for details
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

class Esp32Network : public INetwork {
public:
    TlsHandle connect(const char* host) override {
        esp_tls_t* tls = esp_tls_init();
        if (!tls) return nullptr;
        esp_tls_cfg_t cfg = {};
        cfg.skip_common_name = true;
        cfg.timeout_ms = 30000;
        if (esp_tls_conn_new_sync(host, strlen(host), 443, &cfg, tls) != 1) {
            esp_tls_conn_destroy(tls);
            return nullptr;
        }
        return (TlsHandle)tls;
    }
    bool write(TlsHandle conn, const void* data, size_t len) override {
        esp_tls_t* tls = (esp_tls_t*)conn;
        const char* p = (const char*)data;
        size_t rem = len;
        while (rem > 0) {
            int w = esp_tls_conn_write(tls, p, rem);
            if (w > 0) { p += w; rem -= w; }
            else if (w == ESP_TLS_ERR_SSL_WANT_WRITE) { vTaskDelay(pdMS_TO_TICKS(5)); }
            else return false;
        }
        return true;
    }
    int read(TlsHandle conn, void* buf, size_t len) override {
        return esp_tls_conn_read((esp_tls_t*)conn, buf, len);
    }
    void destroy(TlsHandle conn) override {
        esp_tls_conn_destroy((esp_tls_t*)conn);
    }
};

// ── Thin wrappers for existing call sites ───────────────────────────────────
static inline void oled_clear()                                    { g_hal->display->clear(); }
static inline void oled_pixel(int x, int y, bool on)              { g_hal->display->pixel(x, y, on); }
static inline void oled_hline(int x0, int x1, int y)              { g_hal->display->hline(x0, x1, y); }
static inline void oled_rect(int x, int y, int w, int h, bool f)  { g_hal->display->rect(x, y, w, h, f); }
static inline void oled_text(int x, int y, const char* s, int sz=1) { g_hal->display->text(x, y, s, sz); }
static inline int  oled_text_width(const char* s, int sz=1)        { return g_hal->display->text_width(s, sz); }
static inline void oled_flush()                                    { g_hal->display->flush(); }

// ── SD card ─────────────────────────────────────────────────────────────────
static sdmmc_card_t *g_card = nullptr;
static sdmmc_host_t  g_sd_host;
static sdspi_device_config_t g_slot_config;
static spi_host_device_t g_spi_host = SPI2_HOST;

// FATFS mount handle
static const char *SD_MOUNT = "/sdcard";
static bool g_fatfs_mounted = false;
static BYTE g_fatfs_pdrv = 0xFF;  // FATFS drive number, persists across mount/unmount

// ── SD/SPI mutex ────────────────────────────────────────────────────────────
static SemaphoreHandle_t g_sd_mutex = nullptr;
static volatile bool     g_sd_ready = false;

// ── Harvest timing ──────────────────────────────────────────────────────────
#define QUIET_WINDOW_MS     15000UL
#define DISPLAY_INTERVAL_MS  1000UL

static volatile bool     g_hostConnected    = false;
static volatile bool     g_hostWasConnected = false;
static volatile uint32_t g_lastIoMs         = 0;
static volatile uint32_t g_lastWriteMs      = 0;
static volatile bool     g_writeDetected    = false;
static volatile bool     g_harvesting       = false;
static volatile bool     g_msc_ejected      = false;  // soft eject for harvest

static uint16_t g_filesQueued    = 0;
static uint16_t g_filesUploaded  = 0;
static float    g_hostWrittenMb  = 0.0f;
static float    g_mbQueued       = 0.0f;
static float    g_mbUploaded     = 0.0f;
static float    g_lastUploadKBps = 0.0f;  // speed of last upload for STATUS display
static float    g_sdTotalMb      = 0.0f;
static float    g_sdUsedMb       = 0.0f; // updated periodically for display
static volatile bool g_splashActive = true; // hold splash screen on boot
static volatile bool g_otaActive    = false; // suppress display during OTA download
static bool          g_s3CookieActive = false; // S3 cookie overrides harvest cookie this session
static float    g_uploadingMb    = 0.0f; // live progress of current file upload
static volatile bool g_tlsActive = false; // suppress +++ escape during TLS
static float    g_uploadBaseMb   = 0.0f; // base offset for multipart (completed parts)
static float    g_usbWriteKBps   = 0.0f; // live USB write speed for display
static float    g_uploadKBps     = 0.0f; // live upload speed for display
static uint32_t g_lastDisplayMs  = 0;
static uint32_t g_lastHarvestMs  = 0;
static uint32_t g_harvestCoolMs  = 30000;

static TaskHandle_t g_upload_task  = nullptr;
static TaskHandle_t g_harvest_task = nullptr;
static uint32_t     g_card_sectors = 0;

// Cap USB-visible capacity at 8 GB (aircraft expects 4-16 GB FAT32 drive)
#define MSC_MAX_SECTORS  ((uint32_t)(8ULL * 1024 * 1024 * 1024 / 512))  // 16,777,216
static uint32_t msc_visible_sectors() {
    return g_card_sectors < MSC_MAX_SECTORS ? g_card_sectors : MSC_MAX_SECTORS;
}

// Deferred harvest log
static char g_harvest_log[512] = "";
static char g_sd_error[128] = "";  // persists SD init errors for STATUS display

// ── WiFi / captive portal ───────────────────────────────────────────────────
#define WIFI_AP_SSID            "AirBridge"
#define WIFI_CONNECT_TIMEOUT_MS  10000UL
#define WIFI_GRACE_MS            60000UL
#define AP_RETRY_MS             300000UL
// MAX_KNOWN_NETS, NetCred, loadKnownNets, saveNetwork — moved to airbridge_wifi_creds.h

static volatile bool g_netConnected    = false;
static volatile bool g_apMode          = false;
static char          g_wifiLabel[22]   = "No WiFi";
static int8_t        g_wifiBars        = 0;

static volatile bool    g_portal_connected = false;
static TaskHandle_t     g_wifi_task        = nullptr;
static esp_netif_t     *g_sta_netif        = nullptr;
static esp_netif_t     *g_ap_netif         = nullptr;
static EventGroupHandle_t g_wifi_events    = nullptr;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WIFI_GOT_IP_BIT     BIT2

// Cached WiFi scan results
#define MAX_SCAN_RESULTS 15
static char   g_scan_ssids[MAX_SCAN_RESULTS][33];
static int8_t g_scan_rssi[MAX_SCAN_RESULTS];
static bool   g_scan_enc[MAX_SCAN_RESULTS];
static int    g_scan_count = 0;

// Captive portal HTTP server + DNS socket
static httpd_handle_t g_httpd = nullptr;
static int g_dns_sock = -1;
static TaskHandle_t g_dns_task = nullptr;

// ── Time tracking (synced from cellular network) ────────────────────────────
static uint32_t g_bootEpoch   = 0;  // unix epoch at boot (from AT+CCLK)
static uint32_t g_bootMs      = 0;  // millis() when epoch was captured
static char     g_logFileName[48] = "";  // per-session log name (set after time sync)
static uint32_t g_bootCount      = 0;   // persistent boot counter from NVS

// ── Cellular modem (SIM7600) ────────────────────────────────────────────────
static esp_netif_t      *g_ppp_netif    = nullptr;
static TaskHandle_t      g_modem_task   = nullptr;
static volatile bool     g_pppConnected = false;
static volatile bool     g_pppNeedsReconnect = false;
static volatile bool     g_modemReady   = false;
static char              g_modemOp[32]  = "";
static int               g_modemRssi    = 99;

// ── CDC CLI ─────────────────────────────────────────────────────────────────
static char g_cli_buf[128];
static int  g_cli_len = 0;

// ── Forward declarations ────────────────────────────────────────────────────
static void processCLI(const char* cmd);
static void log_write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void doUpdateDisplay();
static void disp(const char* line1, const char* line2 = nullptr);
static bool sd_mount_fatfs();
static void sd_unmount_fatfs();

// ── TinyUSB MSC callbacks ───────────────────────────────────────────────────
// These run in the TinyUSB task context (small stack), so use short mutex timeout.

extern "C" {

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    memcpy(vendor_id, "AirBridg", 8);
    memcpy(product_id, "SD Storage      ", 16);
    memcpy(product_rev, "1.0 ", 4);
    log_write("SCSI: INQUIRY");
}

static uint32_t g_tur_count = 0;
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    g_tur_count++;
    // Log first TUR and any failures (TUR is polled frequently so don't log every one)
    if (g_tur_count == 1) log_write("SCSI: first TEST_UNIT_READY");
    // Only report not-ready for real eject or SD failure — NOT during harvest.
    // Reporting not-ready during harvest causes the host to see media removal,
    // which marks the filesystem dirty and breaks drag-and-drop in file managers.
    if (g_msc_ejected || !g_sd_ready) {
        if (g_tur_count <= 3) log_write("SCSI: TUR not-ready (ejected=%d sd=%d)", g_msc_ejected, g_sd_ready);
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = msc_visible_sectors();
    *block_size  = 512;
    log_write("SCSI: READ_CAPACITY %lu sectors (real=%lu)", (unsigned long)*block_count, (unsigned long)g_card_sectors);
}

static uint32_t g_read10_count = 0;
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           void *buffer, uint32_t bufsize) {
    (void)lun; (void)offset;
    g_read10_count++;
    if (g_read10_count == 1) log_write("SCSI: first READ10 lba=%lu len=%lu", (unsigned long)lba, (unsigned long)bufsize);
    if (lba + bufsize / 512 > msc_visible_sectors()) return -1;
    if (!g_sd_ready || g_harvesting || g_msc_ejected) return -1;
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    esp_err_t err = sdmmc_read_sectors(g_card, buffer, lba, bufsize / 512);
    xSemaphoreGive(g_sd_mutex);
    if (err == ESP_OK) g_lastIoMs = millis();
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

static uint32_t g_msc_write_calls = 0;
static uint32_t g_msc_write_reject = 0;
int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                            uint8_t *buffer, uint32_t bufsize) {
    (void)lun; (void)offset;
    g_msc_write_calls++;
    if (g_msc_write_calls == 1) log_write("SCSI: first WRITE10 lba=%lu len=%lu", (unsigned long)lba, (unsigned long)bufsize);
    if (lba + bufsize / 512 > msc_visible_sectors()) { g_msc_write_reject++; return -1; }
    if (!g_sd_ready || g_harvesting || g_msc_ejected) { g_msc_write_reject++; return -1; }
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    esp_err_t err = sdmmc_write_sectors(g_card, buffer, lba, bufsize / 512);
    xSemaphoreGive(g_sd_mutex);
    if (err == ESP_OK) {
        g_lastWriteMs      = millis();
        g_lastIoMs         = g_lastWriteMs;
        g_writeDetected    = true;
        g_hostWasConnected = true;  // any write proves a host is connected
        g_hostWrittenMb   += bufsize / 1e6f;
    }
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

// TinyUSB handles TEST_UNIT_READY, START_STOP, READ_CAPACITY, INQUIRY, MODE_SENSE(6)
// as built-in commands. tud_msc_scsi_cb handles the rest (including MODE_SENSE(10)).
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                         void *buffer, uint16_t bufsize) {
    (void)lun;

    switch (scsi_cmd[0]) {
        case 0x1E:  // PREVENT ALLOW MEDIUM REMOVAL
            log_write("SCSI: PREVENT_ALLOW_MEDIUM_REMOVAL");
            tud_msc_set_sense(lun, 0, 0, 0);
            return 0;

        case 0x35:  // SYNCHRONIZE CACHE (host flushes write cache)
            log_write("SCSI: SYNCHRONIZE_CACHE");
            tud_msc_set_sense(lun, 0, 0, 0);
            return 0;

        case 0x23: { // READ FORMAT CAPACITIES
            log_write("SCSI: READ_FORMAT_CAPACITIES");
            uint8_t* p = (uint8_t*)buffer;
            if (bufsize >= 12) {
                memset(p, 0, 12);
                p[3] = 8;  // capacity list length
                // Current capacity (capped for avionics compatibility)
                uint32_t sectors = msc_visible_sectors();
                p[4] = (sectors >> 24) & 0xFF;
                p[5] = (sectors >> 16) & 0xFF;
                p[6] = (sectors >>  8) & 0xFF;
                p[7] = sectors & 0xFF;
                p[8] = 0x02;  // formatted media
                p[9] = 0;     // block size 512
                p[10] = 0x02;
                p[11] = 0x00;
                tud_msc_set_sense(lun, 0, 0, 0);
                return 12;
            }
            tud_msc_set_sense(lun, 0, 0, 0);
            return 0;
        }

        case 0x5A: { // MODE SENSE(10) — not handled by TinyUSB (only 0x1A/6-byte is)
            log_write("SCSI: MODE_SENSE_10 page=0x%02X", scsi_cmd[2] & 0x3F);
            // Return 8-byte header: not write-protected, no block descriptors, no mode pages
            if (bufsize >= 8) {
                uint8_t* p = (uint8_t*)buffer;
                memset(p, 0, 8);
                p[0] = 0;  // Mode data length MSB
                p[1] = 6;  // Mode data length LSB (6 bytes follow)
                // p[2] = 0: medium type (default)
                // p[3] = 0: device-specific parameter (bit7=0 = not write-protected)
                // p[4..5] = 0: reserved
                // p[6..7] = 0: block descriptor length (none)
                tud_msc_set_sense(lun, 0, 0, 0);
                return 8;
            }
            tud_msc_set_sense(lun, 0, 0, 0);
            return 0;
        }

        default:
            // Log unknown SCSI commands for debugging avionics compatibility
            log_write("SCSI unknown cmd=0x%02X", scsi_cmd[0]);
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject) {
        g_hostConnected = start;
        if (start) g_hostWasConnected = true;
        log_write("USB host %s", start ? "connected" : "ejected");
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return !g_harvesting && !g_msc_ejected && g_sd_ready;
}

// 1200-baud touch handler: reboot into ROM USB bootloader for flashing.
// Replicates Arduino's behavior so `pio run -t upload` works without BOOT+RESET.
#include "soc/rtc_cntl_reg.h"
void cdc_line_coding_callback(int itf, cdcacm_event_t *event) {
    (void)itf; (void)event;
    cdc_line_coding_t coding;
    tud_cdc_n_get_line_coding(0, &coding);  // void return
    {
        if (coding.bit_rate == 1200) {
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_restart();
        }
    }
}

// CDC RX callback for serial CLI — called by esp_tinyusb CDC ACM wrapper
void cdc_rx_callback(int itf, cdcacm_event_t *event) {
    (void)itf; (void)event;
    uint8_t buf[64];
    size_t rx_size = 0;
    while (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &rx_size) == ESP_OK && rx_size > 0) {
        for (size_t i = 0; i < rx_size; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (g_cli_len > 0) {
                    g_cli_buf[g_cli_len] = 0;
                    processCLI(g_cli_buf);
                    g_cli_len = 0;
                }
            } else if (g_cli_len < (int)sizeof(g_cli_buf) - 1) {
                g_cli_buf[g_cli_len++] = c;
            }
        }
        rx_size = 0;
    }
}

}  // extern "C"

// ── CDC print helper ────────────────────────────────────────────────────────
// Wrapper to print to USB CDC (similar to Serial.print)
static void cdc_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void cdc_printf(const char* fmt, ...) {
    if (g_msc_only) return;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len > 0 && tud_cdc_connected()) {
        tud_cdc_write(buf, len);
        tud_cdc_write_flush();
    }
}

// ── File-based logger (readable from USB) ───────────────────────────────────
// Buffers log entries in RAM. Flushed to /sdcard/airbridge.log during harvest
// (when we have exclusive FATFS access — can't write while MSC is active).
#define LOG_BUF_SIZE 8192
static char g_log_buf[LOG_BUF_SIZE];
static int  g_log_len = 0;
static SemaphoreHandle_t g_log_mutex = nullptr;

static void log_init() {
    g_log_mutex = xSemaphoreCreateMutex();
    g_log_len = 0;
}

// Buffer a log entry in RAM (call anytime)
static void log_write(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_write(const char* fmt, ...) {
    if (!g_log_mutex) return;
    if (xSemaphoreTake(g_log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;

    // Prepend timestamp: real time if synced, otherwise uptime
    int avail = LOG_BUF_SIZE - g_log_len;
    if (avail > 24) {
        int n;
        if (g_bootEpoch > 0) {
            uint32_t now = g_bootEpoch + (millis() - g_bootMs) / 1000;
            time_t t = (time_t)now;
            struct tm tm;
            gmtime_r(&t, &tm);
            n = snprintf(g_log_buf + g_log_len, avail, "[%02d/%02d %02d:%02d:%02d] ",
                         tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        } else {
            n = snprintf(g_log_buf + g_log_len, avail, "[+%lus] ", (unsigned long)(millis() / 1000));
        }
        g_log_len += n;
        avail -= n;
    }

    if (avail > 1) {
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(g_log_buf + g_log_len, avail, fmt, args);
        va_end(args);
        if (n > 0) {
            g_log_len += (n < avail) ? n : avail - 1;
        }
    }

    // Add newline
    if (g_log_len < LOG_BUF_SIZE - 1) {
        g_log_buf[g_log_len++] = '\n';
    }

    // If buffer is getting full, discard oldest half
    if (g_log_len > LOG_BUF_SIZE - 256) {
        int half = g_log_len / 2;
        // Find next newline after halfway point
        while (half < g_log_len && g_log_buf[half] != '\n') half++;
        if (half < g_log_len) half++;
        memmove(g_log_buf, g_log_buf + half, g_log_len - half);
        g_log_len -= half;
    }

    xSemaphoreGive(g_log_mutex);
}

// Flush buffered log to SD card file (call ONLY with exclusive FATFS access)
static void log_flush_to_sd() {
    if (!g_log_mutex || g_log_len == 0) return;
    if (xSemaphoreTake(g_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;

    FILE* f = fopen("/sdcard/airbridge.log", "a");
    if (f) {
        // Truncate if file is too large
        fseek(f, 0, SEEK_END);
        if (ftell(f) > 64 * 1024) {
            fclose(f);
            f = fopen("/sdcard/airbridge.log", "w");
        }
        if (f) {
            fwrite(g_log_buf, 1, g_log_len, f);
            fclose(f);
        }
    }
    g_log_len = 0;
    xSemaphoreGive(g_log_mutex);
}

// ── SD card init ────────────────────────────────────────────────────────────
// Single init: mounts FATFS which also initializes the card.
// g_card is set by esp_vfs_fat_sdspi_mount and used for raw MSC sector access.
static bool sd_init() {
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = PIN_SD_MOSI;
    bus_cfg.miso_io_num = PIN_SD_MISO;
    bus_cfg.sclk_io_num = PIN_SD_SCK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 8192;

    esp_err_t ret = spi_bus_initialize(g_spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    // Report SPI init result for debugging
    int spi_ret = (int)ret;
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        snprintf(g_sd_error, sizeof(g_sd_error), "SPI: %s", esp_err_to_name(ret));
        return false;
    }

    g_sd_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    g_sd_host.slot = g_spi_host;
    g_sd_host.max_freq_khz = 10000;

    g_slot_config = (sdspi_device_config_t)SDSPI_DEVICE_CONFIG_DEFAULT();
    g_slot_config.gpio_cs = (gpio_num_t)PIN_SD_CS;
    g_slot_config.host_id = g_spi_host;

    // Try manual init first to diagnose
    int card_handle = -1;
    ret = sdspi_host_init_device(&g_slot_config, &card_handle);
    int dev_ret = (int)ret;

    sdmmc_card_t *tmp_card = nullptr;
    int card_ret = -1;
    if (ret == ESP_OK) {
        g_sd_host.slot = card_handle;
        tmp_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
        if (tmp_card) {
            ret = sdmmc_card_init(&g_sd_host, tmp_card);
            card_ret = (int)ret;
        }
    }

    if (dev_ret != 0 || card_ret != 0) {
        snprintf(g_sd_error, sizeof(g_sd_error), "spi=%d dev=%d card=%d",
                 spi_ret, dev_ret, card_ret);
        if (tmp_card) free(tmp_card);
        return false;
    }

    // Card works — now clean up and let esp_vfs_fat_sdspi_mount handle FATFS.
    // We need to remove the SPI device first since sdspi_mount will re-add it.
    free(tmp_card); tmp_card = nullptr;
    sdspi_host_remove_device(card_handle);

    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;  // card already formatted
    mount_cfg.max_files = 5;
    mount_cfg.allocation_unit_size = 16 * 1024;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &g_sd_host,
                                   &g_slot_config, &mount_cfg, &g_card);
    if (ret == ESP_OK) {
        g_fatfs_mounted = true;
        g_card_sectors = g_card->csd.capacity;
    } else {
        snprintf(g_sd_error, sizeof(g_sd_error), "mount: %s", esp_err_to_name(ret));
        // Fall back: re-init for MSC-only
        sdspi_host_init_device(&g_slot_config, &card_handle);
        g_sd_host.slot = card_handle;
        g_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
        if (g_card) {
            sdmmc_card_init(&g_sd_host, g_card);
            g_card_sectors = g_card->csd.capacity;
        }
    }

    ESP_LOGI(TAG, "SD: %lu sectors, fatfs=%s",
             (unsigned long)g_card_sectors, g_fatfs_mounted ? "ok" : "FAIL");
    return true;
}

static bool sd_mount_fatfs() {
    if (g_fatfs_mounted) return true;

    esp_vfs_fat_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files = 5;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &g_sd_host,
                                             &g_slot_config, &mount_cfg, &g_card);
    if (ret != ESP_OK) {
        snprintf(g_sd_error, sizeof(g_sd_error), "remount: %s", esp_err_to_name(ret));
        return false;
    }
    g_fatfs_mounted = true;
    g_card_sectors = g_card->csd.capacity;
    return true;
}

static void sd_unmount_fatfs() {
    if (!g_fatfs_mounted) return;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT, g_card);
    g_fatfs_mounted = false;
}

// Re-initialize card + remount FATFS (used by doHarvest after MSC writes)
static bool sd_reinit_and_mount() {
    sd_unmount_fatfs();
    return sd_mount_fatfs();
}

// ── NVS helpers ─────────────────────────────────────────────────────────────
static void nvs_get_string(nvs_handle_t h, const char* key, char* out, size_t sz) {
    size_t len = sz;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) out[0] = '\0';
}

static void nvs_get_string_dflt(const char* ns, const char* key, char* out, size_t sz,
                                 const char* dflt) {
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sz;
        if (nvs_get_str(h, key, out, &len) != ESP_OK) {
            strlcpy(out, dflt, sz);
        }
        nvs_close(h);
    } else {
        strlcpy(out, dflt, sz);
    }
}

// NetCred, loadKnownNets(), saveNetwork() — moved to airbridge_wifi_creds.h

// ── WiFi event handler ──────────────────────────────────────────────────────
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_CONNECTED:
                xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
                xEventGroupClearBits(g_wifi_events, WIFI_DISCONNECTED_BIT);
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                xEventGroupSetBits(g_wifi_events, WIFI_DISCONNECTED_BIT);
                xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
                g_netConnected = false;
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t* ev = (wifi_event_ap_staconnected_t*)event_data;
                ESP_LOGI(TAG, "AP: station connected (aid=%d)", ev->aid);
                break;
            }
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* ev = (ip_event_got_ip_t*)event_data;
        xEventGroupSetBits(g_wifi_events, WIFI_GOT_IP_BIT);
        g_netConnected = true;
        ESP_LOGI(TAG, "WiFi: got IP " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static bool g_wifiStarted = false;

static void wifi_init() {
    g_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Register IP event handler early (needed for PPP too)
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, nullptr));
    // Defer esp_wifi_init/start to save ~37KB heap for TLS
    // WiFi is started on-demand by wifi_start_deferred() when needed
}

static void wifi_start_deferred() {
    if (g_wifiStarted) return;
    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    g_wifiStarted = true;
    ESP_LOGI(TAG, "WiFi started (heap=%lu)", (unsigned long)esp_get_free_heap_size());
}

// ── WiFi helpers ────────────────────────────────────────────────────────────
// rssiToBars() — moved to airbridge_utils.h

static bool tryConnect(const char* ssid, const char* pass) {
    wifi_config_t wifi_cfg = {};
    strlcpy((char*)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    if (pass[0]) strlcpy((char*)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT);
    esp_wifi_connect();

    // Wait for IP or timeout
    EventBits_t bits = xEventGroupWaitBits(g_wifi_events,
        WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_GOT_IP_BIT) return true;

    esp_wifi_disconnect();
    return false;
}

static bool tryKnownNets() {
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "WiFi: trying '%s'", nets[i].ssid);
        strlcpy(g_wifiLabel, nets[i].ssid, sizeof(g_wifiLabel));
        if (tryConnect(nets[i].ssid, nets[i].pass)) return true;
    }
    return false;
}

// Get current SSID and RSSI from the driver
static void wifi_get_info(char* ssid_out, size_t ssid_sz, int8_t* rssi_out) {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strlcpy(ssid_out, (const char*)ap.ssid, ssid_sz);
        *rssi_out = ap.rssi;
    } else {
        strlcpy(ssid_out, "Unknown", ssid_sz);
        *rssi_out = -100;
    }
}

// Get current IP as string
static void wifi_get_ip_str(char* out, size_t sz) {
    esp_netif_ip_info_t ip_info;
    if (g_sta_netif && esp_netif_get_ip_info(g_sta_netif, &ip_info) == ESP_OK) {
        snprintf(out, sz, IPSTR, IP2STR(&ip_info.ip));
    } else {
        strlcpy(out, "0.0.0.0", sz);
    }
}

// ── WiFi scan ───────────────────────────────────────────────────────────────
static void doScan() {
    ESP_LOGI(TAG, "WiFi: scanning...");
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    esp_wifi_scan_start(&scan_cfg, true);  // blocking scan

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > MAX_SCAN_RESULTS) ap_count = MAX_SCAN_RESULTS;

    wifi_ap_record_t *ap_list = (wifi_ap_record_t*)malloc(ap_count * sizeof(wifi_ap_record_t));
    if (!ap_list) { g_scan_count = 0; return; }

    esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    g_scan_count = ap_count;

    for (int i = 0; i < g_scan_count; i++) {
        strlcpy(g_scan_ssids[i], (const char*)ap_list[i].ssid, sizeof(g_scan_ssids[i]));
        g_scan_rssi[i] = ap_list[i].rssi;
        g_scan_enc[i] = (ap_list[i].authmode != WIFI_AUTH_OPEN);
    }
    free(ap_list);
    ESP_LOGI(TAG, "WiFi: scan found %d networks", g_scan_count);
}

// ── Portal HTML ─────────────────────────────────────────────────────────────
static std::string buildPortalHTML() {
    std::string h;
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
            int bars = (g_scan_rssi[i] >= -55) ? 4 :
                       (g_scan_rssi[i] >= -67) ? 3 :
                       (g_scan_rssi[i] >= -80) ? 2 : 1;
            // Escape single quotes in SSID for HTML attribute
            std::string ssid = g_scan_ssids[i];
            std::string escaped;
            for (char c : ssid) {
                if (c == '\'') escaped += "&#39;";
                else escaped += c;
            }
            h += "<option value='"; h += escaped; h += "'>";
            h += escaped; h += "  ";
            for (int b = 0; b < 4; b++)
                h += (b < bars) ? "\xe2\x96\x88" : "\xe2\x96\x91";
            if (g_scan_enc[i]) h += " \xf0\x9f\x94\x92";
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

static const char SUCCESS_HTML[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>AirBridge</title></head>"
    "<body><h2>Connected!</h2>"
    "<p>AirBridge has joined your WiFi network. You can close this page.</p>"
    "</body></html>";

static std::string buildErrorHTML(const char* ssid) {
    std::string h;
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

// url_decode(), form_field() — moved to airbridge_utils.h

// ── Captive portal HTTP handlers ────────────────────────────────────────────
static esp_err_t portal_get_handler(httpd_req_t *req) {
    std::string html = buildPortalHTML();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html.c_str(), html.length());
    return ESP_OK;
}

static esp_err_t portal_scan_handler(httpd_req_t *req) {
    doScan();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

static esp_err_t portal_configure_handler(httpd_req_t *req) {
    char body[256] = "";
    int recv = httpd_req_recv(req, body, sizeof(body) - 1);
    if (recv <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_FAIL;
    }
    body[recv] = '\0';

    // Prefer manually typed name; fall back to select value
    std::string ssid = form_field(body, "ssid_manual");
    if (ssid.empty()) ssid = form_field(body, "ssid");
    std::string pass = form_field(body, "password");

    if (ssid.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Portal: testing '%s'...", ssid.c_str());

    // Test credentials: configure STA and try to connect while AP stays up
    wifi_config_t wifi_cfg = {};
    strlcpy((char*)wifi_cfg.sta.ssid, ssid.c_str(), sizeof(wifi_cfg.sta.ssid));
    if (!pass.empty()) strlcpy((char*)wifi_cfg.sta.password, pass.c_str(), sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    xEventGroupClearBits(g_wifi_events, WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT);
    esp_wifi_connect();

    // Wait up to 10s for connection
    EventBits_t bits = xEventGroupWaitBits(g_wifi_events,
        WIFI_GOT_IP_BIT | WIFI_DISCONNECTED_BIT,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    if (bits & WIFI_GOT_IP_BIT) {
        saveNetwork(ssid.c_str(), pass.c_str());
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, SUCCESS_HTML, strlen(SUCCESS_HTML));
        g_portal_connected = true;
    } else {
        esp_wifi_disconnect();
        std::string errhtml = buildErrorHTML(ssid.c_str());
        httpd_resp_set_type(req, "text/html");
        httpd_resp_send(req, errhtml.c_str(), errhtml.length());
    }
    return ESP_OK;
}

// Catch-all: redirect to portal root (for captive portal detection)
static esp_err_t portal_redirect_handler(httpd_req_t *req) {
    // Check if this is a known captive-portal probe URL — serve the page directly
    const char* uri = req->uri;
    if (strcmp(uri, "/hotspot-detect.html") == 0 ||
        strcmp(uri, "/library/test/success.html") == 0 ||
        strcmp(uri, "/generate_204") == 0 ||
        strcmp(uri, "/connectivitycheck.html") == 0) {
        return portal_get_handler(req);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

// ── DNS redirect task (captive portal) ──────────────────────────────────────
// Simple DNS server that responds to ALL queries with the AP IP (192.168.4.1)
static void dns_task(void* param) {
    (void)param;
    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (g_dns_sock >= 0) {
        int n = recvfrom(g_dns_sock, buf, sizeof(buf), 0,
                         (struct sockaddr*)&client, &clen);
        if (n < 12) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        // Build DNS response: copy query, set response flags, append answer
        uint8_t resp[600];
        if (n > 500) continue;
        memcpy(resp, buf, n);
        resp[2] = 0x81; resp[3] = 0x80;  // QR=1, AA=1, RA=1
        resp[6] = 0x00; resp[7] = 0x01;  // 1 answer

        // Append answer: pointer to name + A record with 192.168.4.1
        int pos = n;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;  // pointer to name in question
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // type A
        resp[pos++] = 0x00; resp[pos++] = 0x01;  // class IN
        resp[pos++] = 0x00; resp[pos++] = 0x00;
        resp[pos++] = 0x00; resp[pos++] = 0x3C;  // TTL 60
        resp[pos++] = 0x00; resp[pos++] = 0x04;  // data length 4
        resp[pos++] = 192; resp[pos++] = 168;
        resp[pos++] = 4;   resp[pos++] = 1;      // 192.168.4.1

        sendto(g_dns_sock, resp, pos, 0,
               (struct sockaddr*)&client, sizeof(client));
    }
    vTaskDelete(nullptr);
}

// ── Captive portal AP ───────────────────────────────────────────────────────
static void startAP() {
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    // APSTA mode: AP stays up while STA can scan
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap_cfg = {};
    strlcpy((char*)ap_cfg.ap.ssid, WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(WIFI_AP_SSID);
    ap_cfg.ap.channel = 6;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);

    vTaskDelay(pdMS_TO_TICKS(100));

    // Scan for nearby networks
    doScan();

    // Start DNS redirect
    if (g_dns_sock < 0) {
        g_dns_sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(53);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(g_dns_sock, (struct sockaddr*)&addr, sizeof(addr));

        // Set receive timeout so the DNS task loop doesn't block forever
        struct timeval tv = {0, 100000};  // 100ms
        setsockopt(g_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        xTaskCreatePinnedToCore(dns_task, "dns", 4096, nullptr, 1, &g_dns_task, 1);
    }

    // Start HTTP server
    if (!g_httpd) {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 8192;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.max_uri_handlers = 8;

        if (httpd_start(&g_httpd, &config) == ESP_OK) {
            httpd_uri_t uri_root = {"/", HTTP_GET, portal_get_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_root);

            httpd_uri_t uri_scan = {"/scan", HTTP_GET, portal_scan_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_scan);

            httpd_uri_t uri_configure = {"/configure", HTTP_POST, portal_configure_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_configure);

            // Captive portal probe URLs
            httpd_uri_t uri_hotspot = {"/hotspot-detect.html", HTTP_GET, portal_get_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_hotspot);

            httpd_uri_t uri_apple = {"/library/test/success.html", HTTP_GET, portal_get_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_apple);

            httpd_uri_t uri_android1 = {"/generate_204", HTTP_GET, portal_get_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_android1);

            httpd_uri_t uri_android2 = {"/connectivitycheck.html", HTTP_GET, portal_get_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_android2);

            // Catch-all redirect for any other path
            httpd_uri_t uri_catch = {"/*", HTTP_GET, portal_redirect_handler, nullptr};
            httpd_register_uri_handler(g_httpd, &uri_catch);
        }
    }

    g_apMode = true;
    strlcpy(g_wifiLabel, WIFI_AP_SSID, sizeof(g_wifiLabel));
    g_wifiBars = 0;
    ESP_LOGI(TAG, "WiFi: AP started — SSID=AirBridge IP=192.168.4.1");
}

static void stopAP() {
    if (g_httpd) { httpd_stop(g_httpd); g_httpd = nullptr; }
    if (g_dns_sock >= 0) {
        int sock = g_dns_sock;
        g_dns_sock = -1;  // signal DNS task to exit
        close(sock);
        if (g_dns_task) {
            vTaskDelay(pdMS_TO_TICKS(200));  // let DNS task exit
            g_dns_task = nullptr;
        }
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
    g_apMode = false;
    ESP_LOGI(TAG, "WiFi: AP stopped");
}

// ── WiFi task — mirrors Pi upload_worker WiFi/portal state machine ──────────
static void wifiTask(void* param) {
    (void)param;
    // Don't start WiFi if cellular PPP is available — saves ~37KB heap for TLS.
    // Only start WiFi as fallback when there's no cellular.
    for (int i = 0; i < 120; i++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (g_pppConnected) {
            // Cellular is up — skip WiFi entirely to preserve heap
            ESP_LOGI(TAG, "WiFi: skipped — cellular PPP active (heap=%lu)",
                     (unsigned long)esp_get_free_heap_size());
            for (;;) vTaskDelay(pdMS_TO_TICKS(60000)); // sleep forever
        }
    }
    // No cellular after 2 min — start WiFi as fallback
    wifi_start_deferred();

    enum WState { WS_TRY_KNOWN, WS_CONNECTED, WS_AP };
    WState   state     = WS_TRY_KNOWN;
    uint32_t discMs    = 0;
    uint32_t apRetryMs = 0;

    for (;;) {
        switch (state) {

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

        case WS_CONNECTED: {
            // Check if still connected via event group
            EventBits_t bits = xEventGroupGetBits(g_wifi_events);
            if (bits & WIFI_GOT_IP_BIT) {
                if (!g_netConnected) {
                    char ssid[33]; int8_t rssi;
                    wifi_get_info(ssid, sizeof(ssid), &rssi);
                    char ip[20];
                    wifi_get_ip_str(ip, sizeof(ip));
                    ESP_LOGI(TAG, "WiFi: connected to '%s' IP=%s", ssid, ip);
                }
                g_netConnected = true; discMs = 0;
                char ssid[33]; int8_t rssi;
                wifi_get_info(ssid, sizeof(ssid), &rssi);
                strlcpy(g_wifiLabel, ssid, sizeof(g_wifiLabel));
                g_wifiBars = rssiToBars(rssi);
                vTaskDelay(pdMS_TO_TICKS(5000));
            } else {
                g_netConnected = false;
                if (discMs == 0) { discMs = millis(); ESP_LOGI(TAG, "WiFi: connection lost"); }
                strlcpy(g_wifiLabel, "No WiFi", sizeof(g_wifiLabel));
                g_wifiBars = 0;
                state = WS_TRY_KNOWN;
            }
            break;
        }

        case WS_AP: {
            if (!g_apMode) { startAP(); apRetryMs = millis(); }

            // Portal POST handler connected successfully?
            if (g_portal_connected) {
                g_portal_connected = false;
                char ssid[33]; int8_t rssi;
                wifi_get_info(ssid, sizeof(ssid), &rssi);
                ESP_LOGI(TAG, "WiFi: connected via portal to '%s'", ssid);
                stopAP();
                state = WS_CONNECTED; discMs = 0;
                break;
            }

            // Periodic STA retry while in AP mode (every 5 min)
            if (millis() - apRetryMs >= AP_RETRY_MS) {
                ESP_LOGI(TAG, "WiFi: AP-mode STA retry");
                stopAP();
                vTaskDelay(pdMS_TO_TICKS(300));
                if (tryKnownNets()) {
                    state = WS_CONNECTED; discMs = 0;
                } else {
                    state = WS_TRY_KNOWN;
                }
                break;
            }

            vTaskDelay(pdMS_TO_TICKS(10));
            break;
        }
        }
    }
}

// ── Display ─────────────────────────────────────────────────────────────────
// disp() and updateDisplay() moved to airbridge_display.h
static void disp(const char* line1, const char* line2) {
    dispSplash(line1, line2);
}

// _fmtSize() — moved to airbridge_utils.h

// updateDisplay() — rendering logic moved to airbridge_display.h
// This wrapper populates DisplayState from globals.
static DisplayState g_displayState = {};
static void doUpdateDisplay() {
    g_displayState.pppConnected  = g_pppConnected;
    g_displayState.netConnected  = g_netConnected;
    g_displayState.modemReady    = g_modemReady;
    g_displayState.modemRssi     = g_modemRssi;
    strlcpy(g_displayState.modemOp, g_modemOp, sizeof(g_displayState.modemOp));
    strlcpy(g_displayState.wifiLabel, g_wifiLabel, sizeof(g_displayState.wifiLabel));
    g_displayState.wifiBars      = g_wifiBars;
    g_displayState.hostWrittenMb = g_hostWrittenMb;
    g_displayState.mbUploaded    = g_mbUploaded;
    g_displayState.mbQueued      = g_mbQueued;
    g_displayState.uploadingMb   = g_uploadingMb;
    g_displayState.usbWriteKBps  = g_usbWriteKBps;
    g_displayState.uploadKBps    = g_uploadKBps;
    updateDisplay(g_displayState);
}

// ── S3 upload via pre-signed URLs ───────────────────────────────────────────
#define S3_CHUNK_SIZE (5UL * 1024 * 1024)

static char g_apiHost[128] = "";
static char g_apiKey[64]   = "";
static char g_deviceId[16] = "";

static bool s3LoadCreds() {
    if (g_apiHost[0] && g_apiKey[0] && g_deviceId[0]) return true;
    nvs_handle_t h;
    if (nvs_open("s3", NVS_READONLY, &h) != ESP_OK) {
        cdc_printf("S3: no credentials in NVS\r\n");
        return false;
    }
    nvs_get_string(h, "api_host", g_apiHost, sizeof(g_apiHost));
    nvs_get_string(h, "api_key",  g_apiKey,  sizeof(g_apiKey));
    nvs_get_string(h, "device_id", g_deviceId, sizeof(g_deviceId));
    nvs_close(h);
    if (!g_apiHost[0] || !g_apiKey[0]) {
        cdc_printf("S3: no credentials in NVS\r\n");
        return false;
    }
    return true;
}

// ── TLS HTTP helpers ────────────────────────────────────────────────────────
// Read HTTP response, extract body (handle chunked), optionally capture ETag.
static std::string httpReadResponse(esp_tls_t *tls, char* etag = nullptr, size_t etagSz = 0) {
    bool chunked = false;
    char linebuf[512];

    // Read headers line by line
    while (true) {
        int pos = 0;
        while (pos < (int)sizeof(linebuf) - 1) {
            char c;
            int r = esp_tls_conn_read(tls, &c, 1);
            if (r <= 0) goto done_headers;
            linebuf[pos++] = c;
            if (c == '\n') break;
        }
        linebuf[pos] = '\0';

        if (strstr(linebuf, "chunked")) chunked = true;

        // Capture ETag header
        if (etag && etagSz > 0 && strncasecmp(linebuf, "ETag:", 5) == 0) {
            char* val = linebuf + 5;
            while (*val == ' ') val++;
            // Strip trailing whitespace
            char* end = val + strlen(val) - 1;
            while (end > val && (*end == '\r' || *end == '\n' || *end == ' ')) *end-- = '\0';
            // Strip surrounding quotes
            if (val[0] == '"') {
                val++;
                char* eq = strrchr(val, '"');
                if (eq) *eq = '\0';
            }
            strlcpy(etag, val, etagSz);
        }

        // End of headers
        if (pos <= 2 && (linebuf[0] == '\r' || linebuf[0] == '\n')) break;
    }
done_headers:

    // Read body
    std::string raw;
    char rbuf[1024];
    uint32_t t1 = millis();
    while (millis() - t1 < 10000) {
        int r = esp_tls_conn_read(tls, rbuf, sizeof(rbuf));
        if (r > 0) { raw.append(rbuf, r); t1 = millis(); }
        else if (r == 0) break;  // connection closed
        else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(50));
        } else break;
    }

    if (!chunked) return raw;
    return dechunk(raw);
}

// Connect TLS to host:443, returns esp_tls handle or nullptr.
// Wrapper: destroy TLS connection and clear the +++ suppression flag
static void tls_destroy(esp_tls_t* tls) {
    esp_tls_conn_destroy(tls);
    g_tlsActive = false;
}

static esp_tls_t* tls_connect(const char* host) {
    esp_tls_cfg_t cfg = {};
    cfg.skip_common_name = true;
    cfg.use_global_ca_store = false;
    cfg.crt_bundle_attach = nullptr;
    cfg.timeout_ms = 30000;
    // cfg.non_block intentionally NOT set — let TLS block normally

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return nullptr;

    g_tlsActive = true;
    int ret = esp_tls_conn_new_sync(host, strlen(host), 443, &cfg, tls);
    if (ret != 1) {
        int esp_err = 0, mbedtls_err = 0;
        esp_tls_error_handle_t err_handle;
        if (esp_tls_get_error_handle(tls, &err_handle) == ESP_OK && err_handle) {
            esp_err = err_handle->last_error;
            mbedtls_err = err_handle->esp_tls_error_code;
        }
        log_write("TLS fail: host=%s ret=%d esp=0x%x mbed=0x%x heap=%lu",
                  host, ret, esp_err, mbedtls_err, (unsigned long)esp_get_free_heap_size());
        cdc_printf("TLS fail: host=%s ret=%d esp=0x%x mbed=0x%x heap=%lu\r\n",
                   host, ret, esp_err, mbedtls_err, (unsigned long)esp_get_free_heap_size());
        tls_destroy(tls);
        return nullptr;
    }
    return tls;
}

// Write exact bytes to TLS connection
static bool tls_write_all(esp_tls_t* tls, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        int ret = esp_tls_conn_write(tls, data + written, len - written);
        if (ret > 0) written += ret;
        else if (ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(5));
        } else return false;
    }
    return true;
}

// Stream `len` bytes from an open file to TLS connection.
static bool httpStreamChunk(esp_tls_t* tls, FILE* f, uint32_t len) {
    static uint8_t cbuf[8192];  // static: keep off task stack
    uint32_t remaining = len;
    while (remaining > 0) {
        uint32_t toRead = (remaining < sizeof(cbuf)) ? remaining : sizeof(cbuf);
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        size_t n = fread(cbuf, 1, toRead, f);
        xSemaphoreGive(g_sd_mutex);
        if (n == 0) return false;

        size_t sent = 0;
        uint32_t lastProgress = millis();
        while (sent < n) {
            if (millis() - lastProgress > 60000) return false;
            int wr = esp_tls_conn_write(tls, cbuf + sent, n - sent);
            if (wr > 0) { sent += wr; lastProgress = millis(); }
            else if (wr == ESP_TLS_ERR_SSL_WANT_WRITE) vTaskDelay(pdMS_TO_TICKS(5));
            else return false;
        }
        remaining -= n;
        g_uploadingMb = g_uploadBaseMb + (len - remaining) / 1e6f;
    }
    return true;
}

static void s3ClearSession() {
    nvs_handle_t h;
    if (nvs_open("s3up", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

// urlEncode(), jsonStr(), jsonInt(), parseUrl() — moved to airbridge_utils.h

// Make an HTTPS GET request to the API Gateway presign endpoint.
static std::string s3ApiGet(const char* queryParams) {
    esp_tls_t* tls = tls_connect(g_apiHost);
    if (!tls) {
        cdc_printf("S3: TLS connect failed to %s", g_apiHost);
        return "";
    }
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET /prod/presign?%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "Connection: close\r\n\r\n",
        queryParams, g_apiHost, g_apiKey);
    if (!tls_write_all(tls, req, rlen)) {
        tls_destroy(tls);
        return "";
    }
    std::string resp = httpReadResponse(tls);
    tls_destroy(tls);
    return resp;
}

// Make an HTTPS POST to the API Gateway /complete endpoint.
static bool s3ApiComplete(const char* uploadId, const char* key,
                          const char* partsJson) {
    esp_tls_t* tls = tls_connect(g_apiHost);
    if (!tls) {
        cdc_printf("S3: TLS connect failed (complete)\r\n");
        return false;
    }
    char body[2048];
    snprintf(body, sizeof(body),
        "{\"upload_id\":\"%s\",\"key\":\"%s\",\"parts\":[%s]}",
        uploadId, key, partsJson);
    int bodyLen = strlen(body);

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "POST /prod/complete HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        g_apiHost, g_apiKey, bodyLen);

    if (!tls_write_all(tls, hdr, hlen) || !tls_write_all(tls, body, bodyLen)) {
        tls_destroy(tls);
        return false;
    }

    std::string resp = httpReadResponse(tls);
    tls_destroy(tls);
    bool ok = resp.find("\"ok\"") != std::string::npos;
    if (!ok) cdc_printf("S3: complete failed: %.200s", resp.c_str());
    return ok;
}

// ── OTA firmware update ─────────────────────────────────────────────────────

// versionNewer() — moved to airbridge_utils.h

static char g_otaTargetVer[16] = "";

static void otaDisplayProgress(int pct, uint32_t received, uint32_t total) {
    oled_clear();
    oled_text(14, 0, "AirBridge", 2);
    oled_hline(0, 127, 20);
    char title[24];
    if (g_otaTargetVer[0])
        snprintf(title, sizeof(title), "OTA v%s", g_otaTargetVer);
    else
        strlcpy(title, "OTA Update", sizeof(title));
    int tw = oled_text_width(title);
    oled_text((128 - tw) / 2, 24, title);
    // Progress bar: 108px wide at y=38
    oled_rect(10, 38, 108, 10, false);
    int fillW = (pct * 106) / 100;
    if (fillW > 0) oled_rect(11, 39, fillW, 8, true);
    char line[28];
    snprintf(line, sizeof(line), "%d%%  %.0fKB/%.0fKB", pct,
             received / 1024.0f, total / 1024.0f);
    int w = oled_text_width(line);
    oled_text((128 - w) / 2, 52, line);
    oled_flush();
}

static bool otaDownloadAndFlash(const char* host, const char* path, uint32_t expectedSize) {
    // Use esp_http_client which properly handles TLS record reassembly,
    // TCP flow control, and timeouts over slow PPP links.
    char url[2048];
    snprintf(url, sizeof(url), "https://%s%s", host, path);

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 120000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 2048;
    config.skip_cert_common_name_check = true;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { log_write("OTA: http_client init failed heap=%lu", (unsigned long)esp_get_free_heap_size()); return false; }

    g_tlsActive = true;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        log_write("OTA: http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        g_tlsActive = false;
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0) content_length = expectedSize;
    int status = esp_http_client_get_status_code(client);
    log_write("OTA: HTTP %d, %d bytes", status, content_length);

    if (status != 200) {
        log_write("OTA: HTTP error %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        g_tlsActive = false;
        return false;
    }

    // Prepare OTA partition
    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) { log_write("OTA: no partition"); esp_http_client_cleanup(client); g_tlsActive = false; return false; }

    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) { log_write("OTA: begin failed"); esp_http_client_cleanup(client); g_tlsActive = false; return false; }

    // Stream body to flash
    char buf[1024];
    uint32_t received = 0;
    while (received < (uint32_t)content_length) {
        int len = esp_http_client_read(client, buf, sizeof(buf));
        if (len > 0) {
            err = esp_ota_write(ota_handle, buf, len);
            if (err != ESP_OK) {
                log_write("OTA: flash write failed at %lu", (unsigned long)received);
                esp_ota_abort(ota_handle);
                esp_http_client_cleanup(client);
                g_tlsActive = false;
                return false;
            }
            received += len;
            if ((received % 16384) < (uint32_t)len)
                otaDisplayProgress((received * 100) / content_length, received, content_length);
        } else if (len == 0) {
            break;  // done
        } else {
            log_write("OTA: read error at %lu", (unsigned long)received);
            break;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    g_tlsActive = false;

    if (received < (uint32_t)content_length) {
        log_write("OTA: incomplete %lu/%d", (unsigned long)received, content_length);
        esp_ota_abort(ota_handle);
        return false;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) { log_write("OTA: end failed: %s", esp_err_to_name(err)); return false; }
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) { log_write("OTA: set_boot failed"); return false; }

    log_write("OTA: success — %lu bytes", (unsigned long)received);
    otaDisplayProgress(100, received, content_length);
    return true;
}

static bool otaDownloadAndFlash_UNUSED(const char* host, const char* path, uint32_t expectedSize) {
    // OLD implementation kept for reference
    // Prepare OTA partition first (flash erase is slow)
    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) { log_write("OTA: no update partition"); return false; }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) { log_write("OTA: begin failed: %s", esp_err_to_name(err)); return false; }

    // Connect and send request
    esp_tls_t* tls = tls_connect(host);
    if (!tls) { log_write("OTA: TLS connect failed"); esp_ota_abort(ota_handle); return false; }

    static char req[3072];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path, host);
    if (!tls_write_all(tls, req, rlen)) { tls_destroy(tls); esp_ota_abort(ota_handle); return false; }

    // Read headers line-by-line (same approach as httpReadResponse, which works over PPP)
    uint32_t contentLength = expectedSize;
    {
        char linebuf[256];
        int pos = 0;
        uint32_t t0 = millis();
        while (millis() - t0 < 15000) {
            int r = esp_tls_conn_read(tls, linebuf + pos, 1);
            if (r == 1) {
                t0 = millis();
                pos++;
                if (pos >= 2 && linebuf[pos-2] == '\r' && linebuf[pos-1] == '\n') {
                    linebuf[pos] = '\0';
                    if (strncasecmp(linebuf, "content-length:", 15) == 0)
                        contentLength = strtoul(linebuf + 15, nullptr, 10);
                    if (pos <= 2) break;  // empty line = end of headers
                    pos = 0;
                }
                if (pos >= (int)sizeof(linebuf) - 1) pos = 0;  // overflow protection
            } else if (r == ESP_TLS_ERR_SSL_WANT_READ) {
                vTaskDelay(pdMS_TO_TICKS(10));
            } else break;
        }
    }
    log_write("OTA: downloading %lu bytes", (unsigned long)contentLength);

    // Stream body to flash (1KB reads, same as httpReadResponse)
    uint32_t received = 0;
    char buf[1024];
    uint32_t t0 = millis();
    while (received < contentLength) {
        int r = esp_tls_conn_read(tls, buf, sizeof(buf));
        if (r > 0) {
            err = esp_ota_write(ota_handle, buf, r);
            if (err != ESP_OK) {
                log_write("OTA: flash write failed at %lu", (unsigned long)received);
                esp_ota_abort(ota_handle); tls_destroy(tls); return false;
            }
            received += r;
            t0 = millis();
            if ((received % 16384) < (uint32_t)r)  // update display every ~16KB
                otaDisplayProgress((received * 100) / contentLength, received, contentLength);
        } else if (r == 0) break;
        else if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (millis() - t0 > 120000) { log_write("OTA: read timeout at %lu", (unsigned long)received); break; }
        } else {
            // Treat any error (including PEER_CLOSE_NOTIFY) as end of data
            break;
        }
    }
    tls_destroy(tls);

    if (received < contentLength) {
        log_write("OTA: incomplete %lu/%lu", (unsigned long)received, (unsigned long)contentLength);
        esp_ota_abort(ota_handle); return false;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) { log_write("OTA: end failed: %s", esp_err_to_name(err)); return false; }
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) { log_write("OTA: set_boot failed: %s", esp_err_to_name(err)); return false; }

    log_write("OTA: success — %lu bytes", (unsigned long)received);
    otaDisplayProgress(100, received, contentLength);
    return true;
}

// Basic connectivity diagnostic — DNS + TCP before TLS
static void netDiag() {
    cdc_printf("DIAG: heap=%lu\r\n", (unsigned long)esp_get_free_heap_size());

    // DNS resolve
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int err = getaddrinfo("httpbin.org", "80", &hints, &res);
    if (err != 0 || !res) {
        cdc_printf("DIAG: DNS FAILED err=%d\r\n", err);
        log_write("DIAG: DNS FAILED err=%d", err);
        return;
    }
    char ip[16];
    inet_ntoa_r(((struct sockaddr_in*)res->ai_addr)->sin_addr, ip, sizeof(ip));
    cdc_printf("DIAG: DNS OK httpbin.org -> %s\r\n", ip);
    log_write("DIAG: DNS OK -> %s", ip);

    // Raw TCP connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { cdc_printf("DIAG: socket failed\r\n"); freeaddrinfo(res); return; }
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    err = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (err != 0) {
        cdc_printf("DIAG: TCP connect FAILED err=%d errno=%d\r\n", err, errno);
        log_write("DIAG: TCP FAILED errno=%d", errno);
        close(sock);
        return;
    }
    cdc_printf("DIAG: TCP OK\r\n");

    // Simple HTTP GET
    const char* req = "GET /ip HTTP/1.1\r\nHost: httpbin.org\r\nConnection: close\r\n\r\n";
    send(sock, req, strlen(req), 0);
    char buf[512] = {};
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);
    if (n > 0) {
        buf[n] = '\0';
        cdc_printf("DIAG: HTTP %d bytes: %.120s\r\n", n, buf);
        log_write("DIAG: HTTP OK %d bytes", n);
    } else {
        cdc_printf("DIAG: HTTP recv failed n=%d errno=%d\r\n", n, errno);
        log_write("DIAG: HTTP recv failed n=%d", n);
    }
}

// Returns: 1=updated (staged, needs reboot), 0=up to date, -1=transient error
static int otaCheck() {
    if (!g_netConnected && !g_pppConnected) return -1;
    if (!s3LoadCreds()) return 0;  // no creds = permanent, don't retry
    g_tlsActive = true;  // suppress +++ for entire OTA check

    log_write("OTA: checking for update (fw=%s)", FW_VERSION);

    // Step 1: GET /prod/firmware — check version
    esp_tls_t* tls = tls_connect(g_apiHost);
    if (!tls) { log_write("OTA: TLS connect failed"); return -1; }
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET /prod/firmware HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "Connection: close\r\n\r\n",
        g_apiHost, g_apiKey);
    if (!tls_write_all(tls, req, rlen)) { tls_destroy(tls); return -1; }
    std::string verResp = httpReadResponse(tls);
    tls_destroy(tls);

    if (verResp.empty() || verResp.find("\"error\"") != std::string::npos) {
        log_write("OTA: no firmware available");
        return 0;  // server says no firmware — don't retry
    }

    std::string newVer = jsonStr(verResp, "version");
    int fwSizeInt = jsonInt(verResp, "size");
    uint32_t fwSize = (fwSizeInt > 0) ? (uint32_t)fwSizeInt : 0;

    if (newVer.empty() || !versionNewer(newVer.c_str(), FW_VERSION)) {
        log_write("OTA: up to date (remote=%s, local=%s)", newVer.c_str(), FW_VERSION);
        return 0;  // up to date — don't retry
    }
    log_write("OTA: update available %s -> %s (%lu bytes)", FW_VERSION, newVer.c_str(), (unsigned long)fwSize);

    // Show on OLED
    oled_clear();
    oled_text(14, 0, "AirBridge", 2);
    oled_hline(0, 127, 20);
    char updLine[28];
    snprintf(updLine, sizeof(updLine), "Update: v%s", newVer.c_str());
    int uw = oled_text_width(updLine);
    oled_text((128 - uw) / 2, 28, updLine);
    oled_text(22, 44, "Downloading...");
    oled_flush();
    g_otaActive = true;
    strlcpy(g_otaTargetVer, newVer.c_str(), sizeof(g_otaTargetVer));

    // Step 2: GET /prod/firmware/download — get presigned URL
    tls = tls_connect(g_apiHost);
    if (!tls) { log_write("OTA: TLS connect failed (download)"); g_otaActive = false; return -1; }
    rlen = snprintf(req, sizeof(req),
        "GET /prod/firmware/download HTTP/1.1\r\n"
        "Host: %s\r\n"
        "x-api-key: %s\r\n"
        "Connection: close\r\n\r\n",
        g_apiHost, g_apiKey);
    if (!tls_write_all(tls, req, rlen)) { tls_destroy(tls); g_otaActive = false; return -1; }
    std::string dlResp = httpReadResponse(tls);
    tls_destroy(tls);

    std::string dlUrl = jsonStr(dlResp, "url");
    if (dlUrl.empty()) { log_write("OTA: no download URL"); g_otaActive = false; return -1; }

    // Parse S3 presigned URL
    char s3Host[128];
    static char s3Path[2500];
    if (!parseUrl(dlUrl, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
        log_write("OTA: bad URL (len=%d)", (int)dlUrl.length());
        g_otaActive = false;
        return -1;
    }
    log_write("OTA: downloading from %s path_len=%d size=%lu", s3Host, (int)strlen(s3Path), (unsigned long)fwSize);

    // Step 3: Download and flash
    if (!otaDownloadAndFlash(s3Host, s3Path, fwSize)) {
        log_write("OTA: download/flash failed");
        disp("OTA FAILED", "");
        vTaskDelay(pdMS_TO_TICKS(10000));
        g_otaActive = false;
        return -1;
    }

    // Stage update: mark pending + record version. Applied on next power cycle.
    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ota_status", "pending");
        nvs_commit(h);
        nvs_close(h);
    }

    log_write("OTA: v%s downloaded — ready to apply", newVer.c_str());
    g_otaActive = false;
    return 1;
}

// Upload a file from /sdcard/harvested/<name> to S3 using pre-signed URLs.
static bool s3UploadFile(const char* name) {
    if (!g_netConnected && !g_pppConnected) { cdc_printf("S3: no network\r\n"); return false; }
    if (!s3LoadCreds()) return false;
    g_tlsActive = true;  // suppress +++ for entire upload session

    char fpath[128];
    snprintf(fpath, sizeof(fpath), "%s/harvested/%s", SD_MOUNT, name);

    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE* f = fopen(fpath, "rb");
    uint32_t fileSize = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        fileSize = ftell(f);
        fseek(f, 0, SEEK_SET);
    }
    xSemaphoreGive(g_sd_mutex);
    if (!f || fileSize == 0) {
        cdc_printf("S3: can't open %s", fpath);
        if (f) fclose(f);
        return false;
    }

    static char s3Path[2500];  // shared, STS tokens are long

    // ── Small file: single pre-signed PUT ────────────────────────────────
    if (fileSize <= S3_CHUNK_SIZE) {
        std::string enc = urlEncode(name);
        char query[512];
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s", enc.c_str(), fileSize, g_deviceId);
        std::string resp = s3ApiGet(query);
        std::string url = jsonStr(resp, "\"url\"");
        if (url.empty()) {
            cdc_printf("S3: presign failed: %.200s", resp.c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        char s3Host[128];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            cdc_printf("S3: URL parse failed\r\n");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        esp_tls_t* tls = tls_connect(s3Host);
        if (!tls) {
            cdc_printf("S3: TLS connect failed to %s", s3Host);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        uint32_t xfrStart = millis();
        char hdr[2700];
        int hlen = snprintf(hdr, sizeof(hdr),
            "PUT %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n\r\n",
            s3Path, s3Host, fileSize);
        if (!tls_write_all(tls, hdr, hlen)) {
            cdc_printf("S3: header send failed\r\n");
            tls_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        if (!httpStreamChunk(tls, f, fileSize)) {
            cdc_printf("S3: stream failed\r\n");
            tls_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);

        std::string putResp = httpReadResponse(tls);
        tls_destroy(tls);
        float elapsed = (millis() - xfrStart) / 1000.0f;
        g_lastUploadKBps = elapsed > 0 ? fileSize / 1024.0f / elapsed : 0;
        cdc_printf("S3: uploaded '%s' OK (%u bytes, %.0f KB/s)\r\n", name, fileSize, g_lastUploadKBps);
        log_write("Upload OK: %s %u bytes %.0f KB/s", name, fileSize, g_lastUploadKBps);
        return true;
    }

    // ── Large file: multipart upload ────────────────────────────────────
    char uploadId[256] = "";
    char s3Key[128] = "";
    uint32_t startPart = 1;
    uint32_t totalParts = 0;

    // Check NVS for interrupted session (with retry limit)
    {
        nvs_handle_t h;
        if (nvs_open("s3up", NVS_READWRITE, &h) == ESP_OK) {
            char storedName[64] = "";
            nvs_get_string(h, "name", storedName, sizeof(storedName));
            if (strcmp(storedName, name) == 0) {
                uint32_t retries = 0;
                nvs_get_u32(h, "retries", &retries);
                if (retries >= 3) {
                    // Too many resume failures — start fresh
                    log_write("S3: stale session for %s (retries=%lu), clearing", name, (unsigned long)retries);
                    nvs_erase_all(h); nvs_commit(h);
                } else {
                    nvs_get_string(h, "uid", uploadId, sizeof(uploadId));
                    nvs_get_string(h, "key", s3Key, sizeof(s3Key));
                    uint32_t p = 1; nvs_get_u32(h, "part", &p); startPart = p;
                    uint32_t tp = 0; nvs_get_u32(h, "parts", &tp); totalParts = tp;
                    nvs_set_u32(h, "retries", retries + 1);
                    nvs_commit(h);
                }
            }
            nvs_close(h);
        }
    }

    // Start new multipart upload if no session
    if (!uploadId[0]) {
        std::string enc = urlEncode(name);
        char query[512];
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s", enc.c_str(), fileSize, g_deviceId);
        std::string resp = s3ApiGet(query);

        std::string uid = jsonStr(resp, "\"upload_id\"");
        std::string key = jsonStr(resp, "\"key\"");
        totalParts = jsonInt(resp, "\"parts\"");

        if (uid.empty() || key.empty() || totalParts == 0) {
            cdc_printf("S3: multipart start failed: %.200s", resp.c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        strlcpy(uploadId, uid.c_str(), sizeof(uploadId));
        strlcpy(s3Key, key.c_str(), sizeof(s3Key));
        startPart = 1;

        // Persist session
        nvs_handle_t h;
        if (nvs_open("s3up", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "name", name);
            nvs_set_str(h, "uid", uploadId);
            nvs_set_str(h, "key", s3Key);
            nvs_set_u32(h, "part", 1);
            nvs_set_u32(h, "parts", totalParts);
            nvs_set_u32(h, "size", fileSize);
            nvs_commit(h);
            nvs_close(h);
        }

        cdc_printf("S3: multipart started, %u parts, upload_id=%s", totalParts, uploadId);
    } else {
        cdc_printf("S3: resuming multipart at part %u/%u", startPart, totalParts);
    }

    // Seek file to resume position
    uint32_t resumeOffset = (startPart - 1) * S3_CHUNK_SIZE;
    if (resumeOffset > 0) {
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        fseek(f, resumeOffset, SEEK_SET);
        xSemaphoreGive(g_sd_mutex);
    }

    uint32_t xfrStart = millis();

    // Upload each part
    for (uint32_t partNum = startPart; partNum <= totalParts; partNum++) {
        uint32_t offset = (partNum - 1) * S3_CHUNK_SIZE;
        uint32_t chunkSize = fileSize - offset;
        if (chunkSize > S3_CHUNK_SIZE) chunkSize = S3_CHUNK_SIZE;

        std::string encKey = urlEncode(s3Key);
        char query[1024];
        snprintf(query, sizeof(query), "upload_id=%s&key=%s&part=%u",
                 uploadId, encKey.c_str(), partNum);
        std::string resp = s3ApiGet(query);
        std::string url = jsonStr(resp, "\"url\"");
        if (url.empty()) {
            cdc_printf("S3: presign part %u failed: %.200s", partNum, resp.c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        char s3Host[128];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            cdc_printf("S3: URL parse failed\r\n");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        cdc_printf("S3: part %u URL len=%d path=%d", partNum, (int)url.length(), (int)strlen(s3Path));

        esp_tls_t* tls = tls_connect(s3Host);
        if (!tls) {
            cdc_printf("S3: TLS connect failed to %s (part %u)", s3Host, partNum);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        char hdr[2700];
        int hlen = snprintf(hdr, sizeof(hdr),
            "PUT %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Length: %u\r\n"
            "Connection: close\r\n\r\n",
            s3Path, s3Host, chunkSize);
        if (!tls_write_all(tls, hdr, hlen)) {
            tls_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        if (!httpStreamChunk(tls, f, chunkSize)) {
            cdc_printf("S3: stream failed at part %u", partNum);
            tls_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Update base so next part's progress continues from here
        g_uploadBaseMb = (float)partNum * S3_CHUNK_SIZE / 1e6f;

        char etag[64] = "";
        std::string putResp = httpReadResponse(tls, etag, sizeof(etag));
        tls_destroy(tls);

        if (!etag[0]) {
            cdc_printf("S3: no ETag for part %u, resp(%d): %.300s",
                     partNum, (int)putResp.length(), putResp.c_str());
            s3ClearSession();
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        // Persist progress + etag to NVS
        {
            nvs_handle_t h;
            if (nvs_open("s3up", NVS_READWRITE, &h) == ESP_OK) {
                char etagKey[12]; snprintf(etagKey, sizeof(etagKey), "etag%u", partNum);
                nvs_set_str(h, etagKey, etag);
                nvs_set_u32(h, "part", partNum + 1);
                nvs_set_u32(h, "retries", 0);
                nvs_commit(h);
                nvs_close(h);
            }
        }

        float elapsed = (millis() - xfrStart) / 1000.0f;
        uint32_t totalSent = offset + chunkSize;
        cdc_printf("S3: part %u/%u done (%u/%u bytes, %.0f%%, %.0f KB/s)\r\n",
                 partNum, totalParts, totalSent, fileSize,
                 totalSent * 100.0f / fileSize,
                 elapsed > 0 ? totalSent / 1024.0f / elapsed : 0);
    }
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);

    // ── Complete multipart upload ─────────────────────────────────────
    std::string partsJson;
    {
        nvs_handle_t h;
        if (nvs_open("s3up", NVS_READONLY, &h) == ESP_OK) {
            for (uint32_t i = 1; i <= totalParts; i++) {
                char etagKey[12]; snprintf(etagKey, sizeof(etagKey), "etag%u", i);
                char etag[64] = "";
                nvs_get_string(h, etagKey, etag, sizeof(etag));
                if (i > 1) partsJson += ",";
                char part[128];
                snprintf(part, sizeof(part), "{\"part\":%u,\"etag\":\"%s\"}", i, etag);
                partsJson += part;
            }
            nvs_close(h);
        }
    }

    if (!s3ApiComplete(uploadId, s3Key, partsJson.c_str())) {
        cdc_printf("S3: complete_multipart failed — clearing session");
        s3ClearSession();
        return false;
    }

    s3ClearSession();

    float elapsed = (millis() - xfrStart) / 1000.0f;
    g_lastUploadKBps = elapsed > 0 ? fileSize / 1024.0f / elapsed : 0;
    log_write("Upload OK: %s %u bytes %u parts %.0f KB/s", name, fileSize, totalParts, g_lastUploadKBps);
    cdc_printf("S3: uploaded '%s' OK (%u bytes, %u parts, %.0f KB/s)\r\n",
             name, fileSize, totalParts, g_lastUploadKBps);
    return true;
}

// SKIP_NAMES[], isSkipped() — moved to airbridge_utils.h

// ── Cellular modem task (SIM7600 via raw UART + PPPoS) ──────────────────────

// Send AT command, wait for response, return response string
static int modem_at_cmd(const char* cmd, char* resp, int resp_size, int timeout_ms) {
    // Send command
    uart_write_bytes(UART_NUM_1, cmd, strlen(cmd));
    uart_write_bytes(UART_NUM_1, "\r", 1);

    // Read response with timeout
    int total = 0;
    uint32_t start = millis();
    while ((millis() - start) < (uint32_t)timeout_ms && total < resp_size - 1) {
        uint8_t buf[128];
        int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf),
                                  pdMS_TO_TICKS(100));
        if (len > 0) {
            int copy = std::min(len, resp_size - 1 - total);
            memcpy(resp + total, buf, copy);
            total += copy;
            // Check if we have a final response
            resp[total] = '\0';
            if (strstr(resp, "OK") || strstr(resp, "ERROR") ||
                strstr(resp, "CONNECT")) {
                break;
            }
        }
    }
    resp[total] = '\0';
    return total;
}

// PPPoS output callback — sends PPP frames to modem UART
static uint32_t g_ppp_tx_bytes = 0;
static uint32_t g_ppp_tx_calls = 0;
static esp_err_t modem_ppp_transmit(void* h, void* buffer, size_t len) {
    g_ppp_tx_calls++;
    g_ppp_tx_bytes += len;
    int written = uart_write_bytes(UART_NUM_1, buffer, len);
    return (written == (int)len) ? ESP_OK : ESP_FAIL;
}

// PPP netif driver glue
static esp_err_t modem_post_attach(esp_netif_t* netif, esp_netif_iodriver_handle driver) {
    // Set the driver transmit function
    const esp_netif_driver_ifconfig_t driver_cfg = {
        .handle = driver,
        .transmit = modem_ppp_transmit,
    };
    return esp_netif_set_driver_config(netif, &driver_cfg);
}

static void modem_ip_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data) {
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        cdc_printf("Modem: PPP IP=" IPSTR "\r\n", IP2STR(&event->ip_info.ip));
        log_write("PPP got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        g_pppConnected = true;
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        cdc_printf("Modem: PPP lost IP\r\n");
        log_write("PPP lost IP");
        g_pppConnected = false;
        g_pppNeedsReconnect = true;
        // Don't reset RSSI — it's still valid from modem init or last reconnect
    }
}

static void modemTask(void* param) {
    (void)param;
    vTaskDelay(pdMS_TO_TICKS(500));  // brief settle for UART pins

    // ── Init UART ────────────────────────────────────────────────────────
    cdc_printf("Modem: init UART1 TX=%d RX=%d...\r\n", PIN_MODEM_TX, PIN_MODEM_RX);
    uart_config_t uart_cfg = {};
    uart_cfg.baud_rate  = 115200;
    uart_cfg.data_bits  = UART_DATA_8_BITS;
    uart_cfg.parity     = UART_PARITY_DISABLE;
    uart_cfg.stop_bits  = UART_STOP_BITS_1;
    uart_cfg.flow_ctrl  = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg.source_clk = UART_SCLK_DEFAULT;

    uart_driver_delete(UART_NUM_1);  // clean up any prior install
    esp_err_t err = uart_driver_install(UART_NUM_1, 32768, 0, 0, nullptr, 0);
    if (err != ESP_OK) {
        cdc_printf("Modem: uart_driver_install failed: %s\r\n", esp_err_to_name(err));
        g_modem_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }
    uart_param_config(UART_NUM_1, &uart_cfg);
    uart_set_pin(UART_NUM_1, PIN_MODEM_TX, PIN_MODEM_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // ── Sync with modem ────────────────────────────────────────────────────
    // Modem needs ~15-20s to boot after cold power-on.
    // Try +++ (exit data mode) then AT repeatedly until it responds.
    bool ready = false;
    uart_flush(UART_NUM_1);

    char resp[512];

    // +++ escape (modem may be in PPP data mode from previous ESP32 boot)
    vTaskDelay(pdMS_TO_TICKS(1100));
    uart_write_bytes(UART_NUM_1, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));
    uart_flush(UART_NUM_1);

    // Try AT at 115200, up to 20s (covers modem cold boot)
    for (int i = 0; i < 40 && !ready; i++) {
        int len = modem_at_cmd("AT", resp, sizeof(resp), 500);
        if (len > 0 && strstr(resp, "OK")) {
            ready = true;
            cdc_printf("Modem: found at 115200 (%ds)\r\n", i / 2);
            break;
        }
    }

    // Fallback: try other bauds — modem may be in PPP data mode at high baud
    if (!ready) {
        const int tryBauds[] = { 921600, 460800, 3000000 };
        for (int b = 0; b < 3 && !ready; b++) {
            // First try WITHOUT flow control (modem may not assert CTS in data mode)
            uart_set_baudrate(UART_NUM_1, tryBauds[b]);
            vTaskDelay(pdMS_TO_TICKS(100));
            uart_flush(UART_NUM_1);
            // +++ escape at this baud
            vTaskDelay(pdMS_TO_TICKS(1100));
            uart_write_bytes(UART_NUM_1, "+++", 3);
            vTaskDelay(pdMS_TO_TICKS(1100));
            uart_flush(UART_NUM_1);
            modem_at_cmd("ATH", resp, sizeof(resp), 1000);
            int len = modem_at_cmd("AT", resp, sizeof(resp), 500);
            if (len > 0 && strstr(resp, "OK")) {
                cdc_printf("Modem: found at %d (no FC)\r\n", tryBauds[b]);
                modem_at_cmd("AT+IFC=0,0", resp, sizeof(resp), 1000);
                modem_at_cmd("AT+IPR=115200", resp, sizeof(resp), 1000);
                vTaskDelay(pdMS_TO_TICKS(200));
                ready = true;
                break;
            }
            // Then try WITH flow control
            uart_set_pin(UART_NUM_1, PIN_MODEM_TX, PIN_MODEM_RX,
                         PIN_MODEM_RTS, PIN_MODEM_CTS);
            uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_CTS_RTS, 122);
            uart_flush(UART_NUM_1);
            vTaskDelay(pdMS_TO_TICKS(1100));
            uart_write_bytes(UART_NUM_1, "+++", 3);
            vTaskDelay(pdMS_TO_TICKS(1100));
            uart_flush(UART_NUM_1);
            modem_at_cmd("ATH", resp, sizeof(resp), 1000);
            len = modem_at_cmd("AT", resp, sizeof(resp), 500);
            if (len > 0 && strstr(resp, "OK")) {
                cdc_printf("Modem: found at %d (FC)\r\n", tryBauds[b]);
                modem_at_cmd("AT+IFC=0,0", resp, sizeof(resp), 1000);
                modem_at_cmd("AT+IPR=115200", resp, sizeof(resp), 1000);
                vTaskDelay(pdMS_TO_TICKS(200));
                ready = true;
                break;
            }
            // Reset flow control for next iteration
            uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_DISABLE, 0);
            uart_set_pin(UART_NUM_1, PIN_MODEM_TX, PIN_MODEM_RX,
                         UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        }
        uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_DISABLE, 0);
        uart_set_pin(UART_NUM_1, PIN_MODEM_TX, PIN_MODEM_RX,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        uart_set_baudrate(UART_NUM_1, 115200);
        uart_flush(UART_NUM_1);
    }

    if (ready) {
        cdc_printf("Modem: AT sync OK\r\n");
        log_write("Modem: AT sync OK");
    }

    if (!ready) {
        cdc_printf("Modem: AT sync failed, task exiting\r\n");
        log_write("Modem: AT sync failed");
        uart_driver_delete(UART_NUM_1);
        g_modem_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    g_modemReady = true;

    // ── Reset modem radio to clear stale PPP/PDP state from previous session ─
    // Without this, the modem may still be in data mode from the last boot,
    // causing PPP to fail silently on subsequent connections.
    cdc_printf("Modem: resetting radio...\r\n");
    modem_at_cmd("AT+CFUN=0", resp, sizeof(resp), 3000);
    vTaskDelay(pdMS_TO_TICKS(500));
    modem_at_cmd("AT+CFUN=1", resp, sizeof(resp), 3000);
    vTaskDelay(pdMS_TO_TICKS(1000));
    modem_at_cmd("AT", resp, sizeof(resp), 2000);

    // ── Disable echo ────────────────────────────────────────────────────
    modem_at_cmd("ATE0", resp, sizeof(resp), 1000);
    modem_at_cmd("AT+CTZU=1", resp, sizeof(resp), 500);

    // ── Time sync (deferred if not available yet) ────────────────────────
    // AT+CCLK? returns: +CCLK: "YY/MM/DD,HH:MM:SS±TZ"
    if (modem_at_cmd("AT+CCLK?", resp, sizeof(resp), 2000) > 0) {
        cdc_printf("Modem: CCLK raw: %s", resp);
        char* q = strchr(resp, '"');
        if (q) {
            int yy = 0, mo = 0, dd = 0, hh = 0, mi = 0, ss = 0;
            char tzSign = '+';
            int tzVal = 0;
            // SIM7600 format: "YY/MM/DD,HH:MM:SS"±TZ  (TZ after closing quote)
            sscanf(q + 1, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &hh, &mi, &ss);
            // Find TZ after closing quote
            char* q2 = strchr(q + 1, '"');
            if (q2) sscanf(q2 + 1, "%c%d", &tzSign, &tzVal);
            cdc_printf("Modem: parsed yy=%d mo=%d dd=%d %d:%d:%d tz=%c%d\r\n",
                       yy, mo, dd, hh, mi, ss, tzSign, tzVal);
            if (yy >= 24 && yy <= 50 && mo >= 1 && mo <= 12) {
                struct tm tm = {};
                tm.tm_year = yy + 100;  // years since 1900
                tm.tm_mon  = mo - 1;
                tm.tm_mday = dd;
                tm.tm_hour = hh;
                tm.tm_min  = mi;
                tm.tm_sec  = ss;
                time_t epoch = mktime(&tm);
                // Adjust for timezone (quarter-hours from UTC)
                int tzOffsetSec = tzVal * 15 * 60;
                if (tzSign == '-') tzOffsetSec = -tzOffsetSec;
                epoch -= tzOffsetSec;  // convert local → UTC
                g_bootEpoch = (uint32_t)epoch;
                g_bootMs = millis();
                // Generate dated log filename for this boot session
                struct tm utc;
                gmtime_r(&epoch, &utc);
                snprintf(g_logFileName, sizeof(g_logFileName),
                         "logs/%04lu_%04d%02d%02d_%02d%02d%02d.log",
                         (unsigned long)g_bootCount,
                         utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                         utc.tm_hour, utc.tm_min, utc.tm_sec);
                cdc_printf("Modem: time synced 20%02d-%02d-%02d %02d:%02d:%02d TZ%c%d\r\n",
                           yy, mo, dd, hh, mi, ss, tzSign, tzVal);
                log_write("Time: 20%02d-%02d-%02d %02d:%02d:%02d TZ%c%d",
                          yy, mo, dd, hh, mi, ss, tzSign, tzVal);
            } else {
                cdc_printf("Modem: CCLK invalid (yy=%d mo=%d) — no time sync\r\n", yy, mo);
                log_write("CCLK invalid: %s", q);
            }
        }
    }

    // ── Increase baud rate ──────────────────────────────────────────────
    // OTA downloads now use esp_http_client which handles TLS properly.
    {
        const int bauds[] = { 921600 };
        bool upgraded = false;
        for (int i = 0; i < 5 && !upgraded; i++) {
            cdc_printf("Modem: trying %d baud...\r\n", bauds[i]);
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+IPR=%d", bauds[i]);
            modem_at_cmd(cmd, resp, sizeof(resp), 2000);
            // Modem responds OK at old baud, THEN switches

            vTaskDelay(pdMS_TO_TICKS(200));
            uart_set_baudrate(UART_NUM_1, bauds[i]);
            vTaskDelay(pdMS_TO_TICKS(200));
            uart_flush(UART_NUM_1);

            // Verify at new baud (multiple attempts)
            bool ok = false;
            for (int j = 0; j < 5; j++) {
                int len = modem_at_cmd("AT", resp, sizeof(resp), 1000);
                if (len > 0 && strstr(resp, "OK")) {
                    ok = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            if (ok) {
                cdc_printf("Modem: baud upgraded to %d\r\n", bauds[i]);
                log_write("Modem: baud %d", bauds[i]);
                upgraded = true;

                // Now try enabling HW flow control at the new baud
                modem_at_cmd("AT+IFC=2,2", resp, sizeof(resp), 2000);
                if (strstr(resp, "OK")) {
                    uart_set_pin(UART_NUM_1, PIN_MODEM_TX, PIN_MODEM_RX,
                                 PIN_MODEM_RTS, PIN_MODEM_CTS);
                    uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_CTS_RTS, 122);
                    vTaskDelay(pdMS_TO_TICKS(100));

                    // Verify flow control works
                    int len = modem_at_cmd("AT", resp, sizeof(resp), 2000);
                    if (len > 0 && strstr(resp, "OK")) {
                        cdc_printf("Modem: HW flow control enabled\r\n");
                    } else {
                        // Flow control broke things — disable it
                        cdc_printf("Modem: HW flow control failed, disabling\r\n");
                        uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_DISABLE, 0);
                        uart_set_pin(UART_NUM_1, PIN_MODEM_TX, PIN_MODEM_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
                        modem_at_cmd("AT+IFC=0,0", resp, sizeof(resp), 2000);
                    }
                }
            } else {
                // Failed — modem is at new baud but ESP can't talk to it
                // Try to reset modem baud: send AT+IPR=115200 at the failed baud
                cdc_printf("Modem: %d failed, resetting...\r\n", bauds[i]);
                modem_at_cmd("AT+IPR=115200", resp, sizeof(resp), 2000);
                vTaskDelay(pdMS_TO_TICKS(200));
                uart_set_baudrate(UART_NUM_1, 115200);
                vTaskDelay(pdMS_TO_TICKS(200));
                uart_flush(UART_NUM_1);
                // Verify recovery
                modem_at_cmd("AT", resp, sizeof(resp), 2000);
            }
        }
        if (!upgraded) {
            cdc_printf("Modem: staying at 115200\r\n");
        }
    }

    // ── Quick pre-PPP setup (minimal — defer everything else) ──────────
    modem_at_cmd("AT+CREG=1", resp, sizeof(resp), 1000);   // enable registration URCs
    modem_at_cmd("AT+AUTOCSQ=1,1", resp, sizeof(resp), 1000); // auto RSSI

    // ── Wait for network registration (up to 30s) ────────────────────────
    // CREG stat: 0=not searching, 1=home, 2=searching, 3=denied, 5=roaming
    {
        bool registered = false;
        for (int i = 0; i < 15; i++) {
            modem_at_cmd("AT+CREG?", resp, sizeof(resp), 1000);
            if (strstr(resp, ",1") || strstr(resp, ",5")) {
                registered = true;
                cdc_printf("Modem: registered (%s)\r\n",
                           strstr(resp, ",5") ? "roaming" : "home");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if (!registered) {
            cdc_printf("Modem: registration timeout — dialing anyway\r\n");
            log_write("Modem: CREG timeout — dialing unregistered");
        }
    }

    // ── Read RSSI + operator (after registration for reliable operator name) ─
    modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000);
    {
        char* p = strstr(resp, "+CSQ:");
        if (p) {
            int rssi = 99;
            sscanf(p, "+CSQ: %d", &rssi);
            g_modemRssi = rssi;
            log_write("Modem: RSSI=%d (pre-dial)", rssi);
            cdc_printf("Modem: RSSI=%d\r\n", rssi);
        }
    }
    if (modem_at_cmd("AT+COPS?", resp, sizeof(resp), 2000) > 0) {
        char* q1 = strchr(resp, '"');
        if (q1) {
            char* q2 = strchr(q1 + 1, '"');
            if (q2) {
                int olen = std::min((int)(q2 - q1 - 1), (int)sizeof(g_modemOp) - 1);
                memcpy(g_modemOp, q1 + 1, olen);
                g_modemOp[olen] = '\0';
            }
        }
        cdc_printf("Modem: %s RSSI=%d\r\n", g_modemOp, g_modemRssi);
        log_write("Modem: operator=%s RSSI=%d", g_modemOp, g_modemRssi);
    }

    // ── Set APN ──────────────────────────────────────────────────────────
    modem_at_cmd("AT+CGDCONT=1,\"IP\",\"hologram\"", resp, sizeof(resp), 5000);
    cdc_printf("Modem: APN set: %s", resp);

    // ── Activate PDP context before dialing ──────────────────────────────
    // AT+CGACT removed — let ATD*99# handle PDP activation

    // ── Dial PPP (with retry) ────────────────────────────────────────────
    bool connected = false;
    for (int dialAttempt = 0; dialAttempt < 3 && !connected; dialAttempt++) {
        if (dialAttempt > 0) {
            cdc_printf("Modem: redial attempt %d...\r\n", dialAttempt + 1);
            modem_at_cmd("ATH", resp, sizeof(resp), 2000);
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
        cdc_printf("Modem: dialing PPP (ATD*99#)...\r\n");
        uart_write_bytes(UART_NUM_1, "ATD*99#\r", 8);

        // Wait for CONNECT
        uint32_t t0 = millis();
        char connbuf[256] = "";
        int connlen = 0;
        while (millis() - t0 < 30000) {
            int len = uart_read_bytes(UART_NUM_1, (uint8_t*)connbuf + connlen,
                                      sizeof(connbuf) - 1 - connlen, pdMS_TO_TICKS(500));
            if (len > 0) {
                connlen += len;
                connbuf[connlen] = '\0';
                if (strstr(connbuf, "CONNECT")) {
                    connected = true;
                    cdc_printf("Modem: CONNECT received!\r\n");
                    log_write("Modem: PPP CONNECT");
                    break;
                }
                if (strstr(connbuf, "ERROR") || strstr(connbuf, "NO CARRIER")) {
                    cdc_printf("Modem: dial failed: %s\r\n", connbuf);
                    break;
                }
            }
        }
    }

    // Retry indefinitely if initial PPP dial fails
    while (!connected) {
        cdc_printf("Modem: PPP dial failed — retrying in 30s\r\n");
        log_write("Modem: PPP dial failed — retrying");
        vTaskDelay(pdMS_TO_TICKS(30000));
        // Re-reset radio and retry
        modem_at_cmd("AT+CFUN=0", resp, sizeof(resp), 3000);
        vTaskDelay(pdMS_TO_TICKS(1000));
        modem_at_cmd("AT+CFUN=1", resp, sizeof(resp), 3000);
        vTaskDelay(pdMS_TO_TICKS(5000));
        for (int dialAttempt = 0; dialAttempt < 3 && !connected; dialAttempt++) {
            cdc_printf("Modem: dialing PPP (attempt %d)...\r\n", dialAttempt + 1);
            uart_write_bytes(UART_NUM_1, "ATD*99#\r", 8);
            uint32_t t0 = millis();
            char connbuf[256] = "";
            int connlen = 0;
            while (millis() - t0 < 30000) {
                int len = uart_read_bytes(UART_NUM_1, (uint8_t*)connbuf + connlen,
                                          sizeof(connbuf) - 1 - connlen, pdMS_TO_TICKS(500));
                if (len > 0) {
                    connlen += len;
                    connbuf[connlen] = '\0';
                    if (strstr(connbuf, "CONNECT")) { connected = true; break; }
                    if (strstr(connbuf, "ERROR") || strstr(connbuf, "NO CARRIER")) break;
                }
            }
        }
    }

    // ── Create PPP netif and start PPPoS ─────────────────────────────────
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP,
                               modem_ip_event_handler, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP,
                               modem_ip_event_handler, nullptr);

    // Create a simple driver handle (just needs to be non-null)
    static int driver_handle;
    esp_netif_driver_ifconfig_t driver_cfg = {};
    driver_cfg.handle = &driver_handle;
    driver_cfg.transmit = modem_ppp_transmit;

    const esp_netif_driver_base_t driver_base = {
        .post_attach = modem_post_attach,
    };

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    g_ppp_netif = esp_netif_new(&netif_ppp_config);
    if (!g_ppp_netif) {
        cdc_printf("Modem: failed to create PPP netif\r\n");
        uart_driver_delete(UART_NUM_1);
        g_modem_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    // Attach driver to netif
    esp_netif_attach(g_ppp_netif, (esp_netif_iodriver_handle)&driver_base);

    // Start PPP
    esp_netif_action_start(g_ppp_netif, nullptr, 0, nullptr);
    esp_netif_action_connected(g_ppp_netif, nullptr, 0, nullptr);

    cdc_printf("Modem: PPPoS started, feeding UART data...\r\n");
    log_write("Modem: PPPoS started");

    // ── PPP data pump: UART ↔ PPPoS ──────────────────────────────────────
    // This loop MUST run continuously — all TCP/IP over cellular depends on it.
    // URCs from the modem (e.g., +CSQ, +CREG) appear as ASCII text between PPP frames.
    bool test_launched = false;
    static char urcBuf[128];
    static int urcLen = 0;

    while (true) {
        uint8_t buf[1024];
        int len = uart_read_bytes(UART_NUM_1, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (len > 0) {
            // Scan for URCs in the data stream
            // PPP frames start with 0x7E; ASCII text outside frames = URCs
            for (int i = 0; i < len; i++) {
                if (buf[i] == 0x7E || buf[i] == '\0') {
                    // PPP frame boundary — process any accumulated URC
                    if (urcLen > 2) {
                        urcBuf[urcLen] = '\0';
                        // Parse +CSQ URC (auto RSSI report)
                        char* csq = strstr(urcBuf, "+CSQ:");
                        if (csq) {
                            int rssi = 99, ber = 99;
                            sscanf(csq, "+CSQ: %d,%d", &rssi, &ber);
                            if (rssi != g_modemRssi && rssi != 99) {
                                log_write("RSSI: %d -> %d", g_modemRssi, rssi);
                                g_modemRssi = rssi;
                            }
                        }
                        // Parse +CREG URC (registration change)
                        char* creg = strstr(urcBuf, "+CREG:");
                        if (creg) {
                            int stat = 0;
                            sscanf(creg, "+CREG: %d", &stat);
                            if (stat != 1 && stat != 5) {  // not registered
                                log_write("Modem: registration lost (stat=%d)", stat);
                                cdc_printf("Modem: registration lost (%d)\r\n", stat);
                            }
                        }
                    }
                    urcLen = 0;
                } else if (buf[i] >= 0x20 && buf[i] < 0x7F && urcLen < (int)sizeof(urcBuf) - 1) {
                    // Accumulate printable ASCII (potential URC text)
                    urcBuf[urcLen++] = buf[i];
                } else if (buf[i] == '\r' || buf[i] == '\n') {
                    // Line ending in URC — treat like frame boundary
                    if (urcLen > 2) {
                        urcBuf[urcLen] = '\0';
                        char* csq = strstr(urcBuf, "+CSQ:");
                        if (csq) {
                            int rssi = 99, ber = 99;
                            sscanf(csq, "+CSQ: %d,%d", &rssi, &ber);
                            if (rssi != g_modemRssi && rssi != 99) {
                                log_write("RSSI: %d -> %d", g_modemRssi, rssi);
                                g_modemRssi = rssi;
                            }
                        }
                        char* creg = strstr(urcBuf, "+CREG:");
                        if (creg) {
                            int stat = 0;
                            sscanf(creg, "+CREG: %d", &stat);
                            if (stat != 1 && stat != 5) {
                                log_write("Modem: registration lost (stat=%d)", stat);
                                cdc_printf("Modem: registration lost (%d)\r\n", stat);
                            }
                        }
                    }
                    urcLen = 0;
                }
            }
            esp_netif_receive(g_ppp_netif, buf, len, nullptr);
        }

        // Once PPP is up, set as default route for all outgoing traffic
        if (g_pppConnected && !test_launched) {
            test_launched = true;

            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK) {
                cdc_printf("Modem: PPP ip=" IPSTR " gw=" IPSTR "\r\n",
                    IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));
            }

            struct netif* ppp_lwip_netif = (struct netif*)esp_netif_get_netif_impl(g_ppp_netif);
            if (ppp_lwip_netif) netif_set_default(ppp_lwip_netif);
            esp_netif_set_default_netif(g_ppp_netif);
            cdc_printf("Modem: cellular ready — set as default route\r\n");
            // Set fallback log filename (no time yet — will be renamed if time syncs)
            if (!g_logFileName[0]) {
                snprintf(g_logFileName, sizeof(g_logFileName),
                         "logs/%04lu.log", (unsigned long)g_bootCount);
            }
        }

        // ── Track last PPP data for stale detection ─────────────────────
        static uint32_t lastPppRxMs = 0;
        if (len > 0) lastPppRxMs = millis();

        // Periodic +++ RSSI refresh removed — it disrupts PPP/TLS.
        // RSSI is read during modem init. modemRssiCheck() available for safe gaps.

        // ── Detect PPP connection loss or stuck CONNECT without IP ────────
        bool pppStale = g_pppConnected && lastPppRxMs > 0 &&
                        (millis() - lastPppRxMs) > 30000;
        // If we got CONNECT but no IP within 30s, force reconnect
        if (!g_pppConnected && !g_pppNeedsReconnect && lastPppRxMs > 0 &&
            (millis() - lastPppRxMs) > 30000) {
            log_write("Modem: no IP after CONNECT — forcing reconnect");
            cdc_printf("Modem: no IP — reconnecting\r\n");
            g_pppNeedsReconnect = true;
        }
        if (pppStale || g_pppNeedsReconnect) {
            if (pppStale) {
                cdc_printf("Modem: no PPP data for 30s — reconnecting\r\n");
                log_write("Modem: PPP stale — reconnecting");
            } else {
                cdc_printf("Modem: PPP lost — reconnecting\r\n");
                log_write("Modem: PPP lost — reconnecting");
                vTaskDelay(pdMS_TO_TICKS(5000));  // pause before retry
            }
            g_pppNeedsReconnect = false;
            g_pppConnected = false;
            g_modemRssi = 0;

            // 1. Exit modem data mode (don't touch esp_netif — it crashes)
            vTaskDelay(pdMS_TO_TICKS(1100));
            uart_write_bytes(UART_NUM_1, "+++", 3);
            vTaskDelay(pdMS_TO_TICKS(1100));
            uart_flush(UART_NUM_1);

            char resp[128];

            // 3. Reset modem radio (AT+CFUN=0 off, then AT+CFUN=1 on)
            cdc_printf("Modem: resetting radio...\r\n");
            modem_at_cmd("ATH", resp, sizeof(resp), 2000);
            modem_at_cmd("AT+CFUN=0", resp, sizeof(resp), 5000);
            vTaskDelay(pdMS_TO_TICKS(10000));
            modem_at_cmd("AT+CFUN=1", resp, sizeof(resp), 5000);
            vTaskDelay(pdMS_TO_TICKS(5000));

            // 4. Wait for registration (up to 60s)
            bool registered = false;
            for (int w = 0; w < 30; w++) {
                modem_at_cmd("AT+CREG?", resp, sizeof(resp), 2000);
                if (strstr(resp, ",1") || strstr(resp, ",5")) {
                    registered = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            if (!registered) {
                cdc_printf("Modem: registration timeout — retry in 60s\r\n");
                log_write("Modem: reconnect reg timeout");
                vTaskDelay(pdMS_TO_TICKS(60000));
                g_pppNeedsReconnect = true;
                continue;  // back to data pump loop
            }

            // 5. Read RSSI
            modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000);
            char* p = strstr(resp, "+CSQ:");
            if (p) {
                int rssi = 99;
                sscanf(p, "+CSQ: %d", &rssi);
                if (rssi != 99) g_modemRssi = rssi;
                log_write("RSSI: %d (reconnect)", g_modemRssi);
            }

            // 5b. Re-read operator name for display
            if (modem_at_cmd("AT+COPS?", resp, sizeof(resp), 2000) > 0) {
                char* q1 = strchr(resp, '"');
                if (q1) {
                    char* q2 = strchr(q1 + 1, '"');
                    if (q2) {
                        int olen = std::min((int)(q2 - q1 - 1), (int)sizeof(g_modemOp) - 1);
                        memcpy(g_modemOp, q1 + 1, olen);
                        g_modemOp[olen] = '\0';
                    }
                }
            }

            // 6. Redial PPP (up to 3 attempts)
            bool redialOk = false;
            for (int attempt = 0; attempt < 3 && !redialOk; attempt++) {
                cdc_printf("Modem: dialing PPP (attempt %d)...\r\n", attempt + 1);
                // AT+CGACT removed — let ATD*99# handle PDP activation
                uart_write_bytes(UART_NUM_1, "ATD*99#\r", 8);
                int rd = uart_read_bytes(UART_NUM_1, (uint8_t*)resp,
                            sizeof(resp) - 1, pdMS_TO_TICKS(15000));
                if (rd > 0) {
                    resp[rd] = '\0';
                    if (strstr(resp, "CONNECT")) {
                        // PPP data pump will feed new modem data to esp_netif
                        lastPppRxMs = millis();
                        test_launched = false;
                        redialOk = true;
                        cdc_printf("Modem: PPP CONNECT — negotiating...\r\n");
                        log_write("Modem: PPP redial CONNECT");
                    } else {
                        cdc_printf("Modem: dial failed: %.60s\r\n", resp);
                        modem_at_cmd("ATH", resp, sizeof(resp), 2000);
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                } else {
                    cdc_printf("Modem: dial timeout\r\n");
                    vTaskDelay(pdMS_TO_TICKS(5000));
                }
            }
            if (!redialOk) {
                log_write("Modem: reconnect failed — retry in 60s");
                cdc_printf("Modem: reconnect failed, retry in 60s\r\n");
                vTaskDelay(pdMS_TO_TICKS(60000));
                g_pppNeedsReconnect = true;
            }
        }

        // ── Periodic heap monitoring ─────────────────────────────────────
        {
            static uint32_t lastHeapLogMs = 0;
            if (g_pppConnected && (millis() - lastHeapLogMs) > 120000) {
                lastHeapLogMs = millis();
                log_write("Heap: free=%lu min=%lu",
                          (unsigned long)esp_get_free_heap_size(),
                          (unsigned long)esp_get_minimum_free_heap_size());
            }
        }

        // ── Opportunistic log upload via cellular ─────────────────────────
        // 60s when idle, skip during file uploads. First upload 30s after connect.
        // Each boot session uses a dated filename (logs/YYYYMMDD_HHMMSS.log).
        // Full buffer is PUT each time (S3 overwrites same key with growing content).
        static uint32_t lastLogUploadMs = 0;
        static bool logUpRunning = false;
        static uint32_t pppConnectMs = 0;
        if (g_pppConnected && pppConnectMs == 0) pppConnectMs = millis();

        bool uploading = (g_filesQueued > 0 || g_uploadingMb > 0);
        uint32_t logInterval = uploading ? 0 : 60000;  // skip during uploads
        uint32_t sinceConnect = pppConnectMs ? (millis() - pppConnectMs) : 0;
        bool firstUpload = (lastLogUploadMs == 0 && sinceConnect > 30000);

        if (g_pppConnected && !logUpRunning && g_log_len > 0 && g_logFileName[0] &&
            (firstUpload || (logInterval > 0 && (millis() - lastLogUploadMs) > logInterval))) {
            lastLogUploadMs = millis();
            logUpRunning = true;
            xTaskCreatePinnedToCore([](void*) {
                // All std::string objects must be scoped so their destructors
                // run before vTaskDelete (which does NOT call C++ destructors).
                do {
                    if (!s3LoadCreds()) break;

                    // Snapshot the full log buffer
                    if (xSemaphoreTake(g_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) break;
                    static char logSnap[LOG_BUF_SIZE];
                    int snapLen = g_log_len;
                    memcpy(logSnap, g_log_buf, snapLen);
                    xSemaphoreGive(g_log_mutex);

                    if (snapLen == 0) break;

                    // Use dated session filename
                    std::string enc = urlEncode(g_logFileName);
                    char query[512];
                    snprintf(query, sizeof(query), "file=%s&size=%d&device=%s",
                             enc.c_str(), snapLen, g_deviceId);
                    std::string resp = s3ApiGet(query);
                    std::string url = jsonStr(resp, "\"url\"");
                    if (url.empty()) break;

                    char s3Host[128];
                    static char s3Path[2500];
                    if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) break;

                    esp_tls_t* tls = tls_connect(s3Host);
                    if (!tls) break;

                    char hdr[2700];
                    int hlen = snprintf(hdr, sizeof(hdr),
                        "PUT %s HTTP/1.1\r\n"
                        "Host: %s\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n",
                        s3Path, s3Host, snapLen);

                    bool ok = tls_write_all(tls, hdr, hlen) &&
                              tls_write_all(tls, logSnap, snapLen);
                    if (ok) httpReadResponse(tls);
                    tls_destroy(tls);
                } while (0);
                logUpRunning = false;
                vTaskDelete(nullptr);
            }, "log_up", 8192, nullptr, 2, nullptr, 1);
        }
    }
}

// ── Modem RSSI check (callable from upload task between files) ──────────────
// Escapes PPP → reads CSQ → returns to data mode. Takes ~3s. Safe to call
// only when no TLS connection is active.
static void modemRssiCheck() {
    if (!g_pppConnected || g_tlsActive) return;

    vTaskDelay(pdMS_TO_TICKS(1100));
    uart_write_bytes(UART_NUM_1, "+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));

    char resp[128];
    int drained = uart_read_bytes(UART_NUM_1, (uint8_t*)resp,
                    sizeof(resp) - 1, pdMS_TO_TICKS(1000));
    bool inCmd = false;
    if (drained > 0) { resp[drained] = '\0'; inCmd = (strstr(resp, "OK") != nullptr); }

    if (inCmd) {
        int len = modem_at_cmd("AT+CSQ", resp, sizeof(resp), 2000);
        if (len > 0) {
            char* p = strstr(resp, "+CSQ:");
            if (p) {
                int rssi = 99, ber = 99;
                sscanf(p, "+CSQ: %d,%d", &rssi, &ber);
                if (rssi != g_modemRssi) log_write("RSSI: %d -> %d", g_modemRssi, rssi);
                g_modemRssi = rssi;
            }
        }
        int ato_len = modem_at_cmd("ATO", resp, sizeof(resp), 3000);
        if (ato_len <= 0 || !strstr(resp, "CONNECT")) {
            log_write("Modem: ATO failed — PPP may be broken: %s", resp);
            cdc_printf("Modem: ATO failed: %s\r\n", resp);
        }
    } else {
        cdc_printf("Modem: +++ escape failed\r\n");
    }
}

// ── Upload task ─────────────────────────────────────────────────────────────
static void uploadTask(void* param) {
    (void)param;
    static bool otaDone = false;

    // Wait for network (up to 90s)
    for (int i = 0; i < 90 && !g_netConnected && !g_pppConnected; i++)
        vTaskDelay(pdMS_TO_TICKS(1000));

    // Let network stack stabilize (default route, DNS, etc.)
    if (g_pppConnected) vTaskDelay(pdMS_TO_TICKS(5000));

    // ── OTA check first (highest priority after network) ────────────
    if (!otaDone && (g_netConnected || g_pppConnected)) {
        int otaResult = 0;
        for (int attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) {
                log_write("OTA: retry %d/3 in 10s", attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(10000));
                if (!g_netConnected && !g_pppConnected) break;
            }
            otaResult = otaCheck();  // 1=updated, 0=up to date, -1=error
            g_tlsActive = false;
            if (otaResult >= 0) break;  // success or up-to-date → stop retrying
        }
        otaDone = true;
        bool staged = (otaResult == 1);

        if (staged) {
            // OTA downloaded — reboot immediately unless host is actively writing
            uint32_t lastWr = g_lastWriteMs;
            if (lastWr != 0 && (millis() - lastWr) < QUIET_WINDOW_MS) {
                // Host is writing — wait for idle
                log_write("OTA: waiting for host writes to settle");
                disp("OTA Ready", "Wait for USB...");
                while (millis() - g_lastWriteMs < QUIET_WINDOW_MS) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            log_write("OTA: rebooting to apply update");
            disp("OTA Complete", "Rebooting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
    }

    // ── Check S3 for custom DSU cookie override ─────────────────────
    if ((g_netConnected || g_pppConnected) && s3LoadCreds()) {
        g_tlsActive = true;
        esp_tls_t* tls = tls_connect(g_apiHost);
        if (tls) {
            char req[512];
            int rlen = snprintf(req, sizeof(req),
                "GET /prod/firmware/cookie HTTP/1.1\r\n"
                "Host: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
                g_apiHost, g_apiKey);
            if (tls_write_all(tls, req, rlen)) {
                std::string resp = httpReadResponse(tls);
                // Response is JSON: {"cookie":"<hex>","size":78} or {"error":"..."}
                std::string hexStr = jsonStr(resp, "cookie");
                if (hexStr.size() == 156) {  // 78 bytes × 2 hex chars
                    // Decode hex to binary
                    uint8_t cookie[78];
                    for (int i = 0; i < 78; i++) {
                        char h[3] = { hexStr[i*2], hexStr[i*2+1], 0 };
                        cookie[i] = (uint8_t)strtoul(h, nullptr, 16);
                    }
                    // Verify magic
                    if (cookie[0] == 0xEA && cookie[1] == 0x1E) {
                        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                        char cookiePath[64];
                        snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", SD_MOUNT);
                        FILE* cf = fopen(cookiePath, "wb");
                        if (cf) {
                            fwrite(cookie, 1, 78, cf);
                            fclose(cf);
                            g_s3CookieActive = true;
                            log_write("S3 cookie applied (overrides harvest cookie)");
                            cdc_printf("S3 cookie: applied\r\n");
                        }
                        xSemaphoreGive(g_sd_mutex);
                    }
                } else {
                    cdc_printf("S3 cookie: none\r\n");
                }
            }
            tls_destroy(tls);
        }
        g_tlsActive = false;
    }

    // ── Upload loop ─────────────────────────────────────────────────
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));  // wake on notify or every 15s

        for (;;) {
            if (g_harvesting) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

            char name[64] = "";
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            {
                char dirpath[64];
                snprintf(dirpath, sizeof(dirpath), "%s/harvested", SD_MOUNT);
                DIR* dir = opendir(dirpath);
                if (dir) {
                    struct dirent* ent;
                    while ((ent = readdir(dir)) != nullptr) {
                        if (ent->d_type == DT_DIR) continue;
                        if (ent->d_name[0] == '.') continue;
                        if (strcmp(ent->d_name, "airbridge.log") == 0) continue;
                        // Skip 0-byte files
                        char fullpath[128];
                        snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name);
                        struct stat st;
                        if (stat(fullpath, &st) == 0 && st.st_size == 0) continue;
                        strlcpy(name, ent->d_name, sizeof(name));
                        break;
                    }
                    closedir(dir);
                }
            }
            xSemaphoreGive(g_sd_mutex);

            if (!name[0]) { cdc_printf("Upload: no files in /harvested/\r\n"); break; }

            char path[128];
            snprintf(path, sizeof(path), "%s/harvested/%s", SD_MOUNT, name);
            cdc_printf("Upload: found %s\r\n", path);

            // Get file size before upload
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            float fileMb = 0.0f;
            {
                struct stat st;
                if (stat(path, &st) == 0) fileMb = (float)st.st_size / 1e6f;
            }
            xSemaphoreGive(g_sd_mutex);

            // Wait for network (WiFi or cellular)
            {
                uint32_t waited = 0;
                while (!g_netConnected && !g_pppConnected && waited < 90000) {
                    if (waited == 0) ESP_LOGI(TAG, "Upload: waiting for network...");
                    vTaskDelay(pdMS_TO_TICKS(2000)); waited += 2000;
                }
            }
            if (!g_netConnected && !g_pppConnected) {
                ESP_LOGI(TAG, "Upload: no network after 90s — will retry in 60s");
                vTaskDelay(pdMS_TO_TICKS(60000)); continue;
            }

            cdc_printf("Uploading: %s (%.1f MB) heap=%lu min=%lu\r\n",
                     name, fileMb,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size());
            g_uploadingMb = 0.0f;
            g_uploadBaseMb = 0.0f;
            bool uploaded = s3UploadFile(name);
            g_tlsActive = false;  // re-enable +++ after upload
            g_uploadingMb = 0.0f;
            g_uploadBaseMb = 0.0f;
            if (!uploaded) {
                cdc_printf("Upload failed for %s — retrying in 30s\r\n", name);
                log_write("Upload FAIL: %s", name);
                vTaskDelay(pdMS_TO_TICKS(30000)); continue;
            }

            // Delete the /harvested/ copy and create .done marker
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            remove(path);
            char donePath[192];
            snprintf(donePath, sizeof(donePath), "%s/harvested/.done__%s", SD_MOUNT, name);
            { FILE* m = fopen(donePath, "w"); if (m) fclose(m); }
            xSemaphoreGive(g_sd_mutex);
            ESP_LOGI(TAG, "Uploaded & marked done: %s", name);
            if (g_filesQueued > 0) g_filesQueued--;
            g_filesUploaded++;
            g_mbUploaded += fileMb;
            if (g_mbQueued >= fileMb) g_mbQueued -= fileMb; else g_mbQueued = 0.0f;

            // RSSI check removed — +++ disrupts PPP even between files
        }
        ESP_LOGI(TAG, "Upload idle — %u uploaded", g_filesUploaded);
    }
}

// ── Harvest ─────────────────────────────────────────────────────────────────
static void doHarvest() {
    ESP_LOGI(TAG, "doHarvest: start");
    g_harvesting = true;
    // Don't set g_msc_ejected — that triggers host "media removed" which
    // marks filesystem dirty and breaks file manager drag-and-drop.
    // g_harvesting already blocks MSC read/write callbacks.
    vTaskDelay(pdMS_TO_TICKS(500));
    doUpdateDisplay();

    // Take mutex to exclude uploadTask from SD for entire harvest
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "doHarvest: got mutex, re-initializing SD");

    // Re-initialize card and remount FATFS to get fresh filesystem view
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = sd_reinit_and_mount();
        cdc_printf("doHarvest: reinit attempt %d = %s\r\n", i+1, ok ? "OK" : "FAIL");
        if (!ok) vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!ok) {
        xSemaphoreGive(g_sd_mutex);
        g_writeDetected = false; g_lastWriteMs = 0;
        g_hostWasConnected = false; g_hostConnected = false;
        g_harvesting = false;
        g_msc_ejected = false;
        ESP_LOGI(TAG, "doHarvest: sd reinit failed, media re-inserted");
        return;
    }

    // Ensure /harvested directory exists
    char harvDir[64];
    snprintf(harvDir, sizeof(harvDir), "%s/harvested", SD_MOUNT);
    mkdir(harvDir, 0755);

    // Walk root directory, copy new files to /harvested/
    // Flattens paths: /logs/data.bin -> /harvested/logs__data.bin
    uint16_t count = 0; float usedMb = 0.0f;

    // Stack-based directory walk
    struct DirFrame { DIR* dir; char prefix[80]; char dirpath[80]; };
    DirFrame stack[4];
    int depth = 0;
    snprintf(stack[0].dirpath, sizeof(stack[0].dirpath), "%s", SD_MOUNT);
    stack[0].dir = opendir(stack[0].dirpath);
    stack[0].prefix[0] = '\0';

    cdc_printf("doHarvest: opendir(%s) = %s\r\n", stack[0].dirpath, stack[0].dir ? "ok" : "FAIL");
    if (!stack[0].dir) {
        cdc_printf("doHarvest: can't open root dir\r\n");
        xSemaphoreGive(g_sd_mutex);
        g_writeDetected = false; g_lastWriteMs = 0;
        g_hostWasConnected = false; g_hostConnected = false;
        g_harvesting = false;
        g_msc_ejected = false;
        return;
    }

    while (depth >= 0) {
        struct dirent* ent = readdir(stack[depth].dir);
        if (!ent) {
            closedir(stack[depth].dir);
            depth--;
            continue;
        }

        const char* name = ent->d_name;

        if (isSkipped(name)) continue;

        char fullpath[160];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", stack[depth].dirpath, name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        cdc_printf("doHarvest: found '%s' %lu bytes %s\r\n", name, (unsigned long)st.st_size,
                   S_ISDIR(st.st_mode) ? "DIR" : "FILE");

        if (S_ISDIR(st.st_mode)) {
            if (depth < 3) {
                depth++;
                snprintf(stack[depth].dirpath, sizeof(stack[depth].dirpath),
                         "%s/%s", stack[depth-1].dirpath, name);
                stack[depth].dir = opendir(stack[depth].dirpath);
                if (!stack[depth].dir) { depth--; continue; }
                if (stack[depth-1].prefix[0])
                    snprintf(stack[depth].prefix, sizeof(stack[depth].prefix),
                             "%s__%s", stack[depth-1].prefix, name);
                else
                    strlcpy(stack[depth].prefix, name, sizeof(stack[depth].prefix));
            }
            continue;
        }

        uint32_t fileBytes = st.st_size;
        float fileMb = (float)fileBytes / 1e6f;
        if (fileBytes == 0) continue;

        // Build destination name (flatten subdirs with __)
        char dstName[128];
        flattenPath(stack[depth].prefix, name, dstName, sizeof(dstName));

        // Skip if already uploaded (.done marker exists)
        char donePath[192];
        snprintf(donePath, sizeof(donePath), "%s/harvested/.done__%s", SD_MOUNT, dstName);
        struct stat donestat;
        if (stat(donePath, &donestat) == 0) continue;

        // Skip if already pending upload (same-size copy in /harvested/)
        char dst[192];
        snprintf(dst, sizeof(dst), "%s/harvested/%s", SD_MOUNT, dstName);
        struct stat dststat;
        if (stat(dst, &dststat) == 0) {
            if ((uint32_t)dststat.st_size == fileBytes) continue;
            remove(dst);  // stale/wrong size — replace
        }

        // Copy file
        FILE* sf = fopen(fullpath, "rb");
        FILE* df = fopen(dst, "wb");
        bool copied = false;
        if (sf && df) {
            static uint8_t cpbuf[8192];
            uint32_t rem = fileBytes;
            copied = true;
            while (rem > 0) {
                size_t toRead = (rem < sizeof(cpbuf)) ? rem : sizeof(cpbuf);
                size_t n = fread(cpbuf, 1, toRead, sf);
                if (n == 0 || fwrite(cpbuf, 1, n, df) != n) { copied = false; break; }
                rem -= n;
            }
        }
        if (sf) fclose(sf);
        if (df) fclose(df);
        if (!copied) { remove(dst); }

        if (copied) {
            ESP_LOGI(TAG, "Harvested: %s -> %s (%.1f MB)", fullpath, dstName, fileMb);
            usedMb += fileMb; count++;
        } else {
            ESP_LOGE(TAG, "Harvest copy failed: %s", fullpath);
        }
    }

    xSemaphoreGive(g_sd_mutex);

    g_filesQueued += count;
    if (count > 0) g_mbQueued += usedMb;

    // Update SD used space while we have exclusive access
    {
        FATFS* fs;
        DWORD freeClusters;
        if (f_getfree("0:", &freeClusters, &fs) == FR_OK) {
            DWORD totalSectors = (fs->n_fatent - 2) * fs->csize;
            DWORD freeSectors  = freeClusters * fs->csize;
            g_sdUsedMb = (totalSectors - freeSectors) * 512.0f / 1e6f;
        }
    }

    cdc_printf("doHarvest: done %u file(s) (%.1f MB)\r\n", count, usedMb);
    log_write("Harvest: %u file(s), %.1f MB", count, usedMb);

    // Flush log buffer to SD while we have exclusive FATFS access
    log_flush_to_sd();

    // ── Write DSU cookie (.easdf) after successful harvest ──────────────
    // Skip if an S3 cookie override is active this session.
    // Parse the highest flight number from harvested .eaofh filenames,
    // then write dsuCookie.easdf so the DSU knows where to resume.
    // Filename format: EA500.000243_01218_20260406.eaofh
    if (count > 0 && !g_s3CookieActive) {
        uint32_t maxFlight = 0;
        char dsuSerial[44] = "";
        char harvestDir[64];
        snprintf(harvestDir, sizeof(harvestDir), "%s/harvested", SD_MOUNT);
        DIR* hdir = opendir(harvestDir);
        if (hdir) {
            struct dirent* ent;
            while ((ent = readdir(hdir)) != nullptr) {
                const char* ext = strrchr(ent->d_name, '.');
                if (!ext || strcmp(ext, ".eaofh") != 0) continue;
                // Parse: flightHistory__EA500.000243_01218_20260406.eaofh
                // or: EA500.000243_01218_20260406.eaofh
                char serial[44] = "";
                uint32_t fnum = 0;
                if (parseDsuFilename(ent->d_name, serial, sizeof(serial), &fnum) && fnum > maxFlight) {
                    maxFlight = fnum;
                    strlcpy(dsuSerial, serial, sizeof(dsuSerial));
                }
            }
            closedir(hdir);
        }

        if (maxFlight > 0 && dsuSerial[0]) {
            uint8_t cookie[78];
            buildDsuCookie(dsuSerial, maxFlight, cookie);

            // Write to SD root
            char cookiePath[64];
            snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", SD_MOUNT);
            FILE* cf = fopen(cookiePath, "wb");
            if (cf) {
                fwrite(cookie, 1, 78, cf);
                fclose(cf);
                log_write("Cookie: %s flight %lu", dsuSerial, (unsigned long)maxFlight);
                cdc_printf("Cookie: %s flight %lu\r\n", dsuSerial, (unsigned long)maxFlight);
            }
        }
    }

    g_writeDetected = false; g_lastWriteMs = 0;
    g_hostWasConnected = false; g_hostConnected = false;
    // Note: g_hostWrittenMb NOT reset — it's a session-cumulative display metric

    g_harvesting = false;
    g_msc_ejected = false;
    g_lastHarvestMs = millis();
    g_harvestCoolMs = QUIET_WINDOW_MS;
    ESP_LOGI(TAG, "doHarvest: media re-inserted (%u files, cooldown %us)",
             count, g_harvestCoolMs / 1000);

    if (count > 0 && g_upload_task) xTaskNotifyGive(g_upload_task);
}

static void harvestTask(void* param) {
    (void)param;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        doHarvest();
    }
}

// ── Serial CLI ──────────────────────────────────────────────────────────────
static void processCLI(const char* cmd) {
    if (strncmp(cmd, "SETWIFI ", 8) == 0) {
        const char* rest = cmd + 8;
        const char* sp   = strchr(rest, ' ');
        char ssid[33] = "", pass[65] = "";
        if (sp) {
            int slen = sp - rest;
            strlcpy(ssid, rest, std::min(slen + 1, (int)sizeof(ssid)));
            strlcpy(pass, sp + 1, sizeof(pass));
        } else {
            strlcpy(ssid, rest, sizeof(ssid));
        }
        if (!ssid[0]) { cdc_printf("CLI: SETWIFI <ssid> <pass>\r\n"); return; }
        saveNetwork(ssid, pass);
        cdc_printf("CLI: WiFi saved: '%s'\r\n", ssid);

    } else if (strncmp(cmd, "SETS3 ", 6) == 0) {
        char apiHost[128], apiKey[64];
        if (sscanf(cmd + 6, "%127s %63s", apiHost, apiKey) < 2) {
            cdc_printf("CLI: SETS3 <api_host> <api_key>\r\n"); return;
        }
        nvs_handle_t h;
        if (nvs_open("s3", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "api_host", apiHost);
            nvs_set_str(h, "api_key", apiKey);
            nvs_commit(h);
            nvs_close(h);
        }
        g_apiHost[0] = 0; g_apiKey[0] = 0;
        cdc_printf("CLI: S3 saved: api_host=%s\r\n", apiHost);

    } else if (strcmp(cmd, "STATUS") == 0) {
        char ipStr[20] = "disconnected";
        if (g_netConnected) wifi_get_ip_str(ipStr, sizeof(ipStr));
        cdc_printf("CLI: wifi=%s ap=%s files_q=%u files_up=%u mbq=%.2f mbup=%.2f sd=%s fatfs=%s sectors=%lu usb_visible=%lu\r\n",
            ipStr, g_apMode ? "yes" : "no",
            g_filesQueued, g_filesUploaded,
            g_mbQueued, g_mbUploaded,
            g_sd_ready ? "ok" : "FAIL", g_fatfs_mounted ? "ok" : "FAIL",
            (unsigned long)g_card_sectors, (unsigned long)msc_visible_sectors());
        if (g_lastUploadKBps > 0) cdc_printf("CLI: last_upload=%.0f KB/s\r\n", g_lastUploadKBps);
        cdc_printf("CLI: wr_det=%d wr_mb=%.2f host_was=%d last_wr=%lu cool=%lu\r\n",
            g_writeDetected, g_hostWrittenMb, g_hostWasConnected,
            (unsigned long)g_lastWriteMs, (unsigned long)g_harvestCoolMs);
        cdc_printf("CLI: msc_wr=%lu msc_rej=%lu ejected=%d harvesting=%d\r\n",
            (unsigned long)g_msc_write_calls, (unsigned long)g_msc_write_reject,
            g_msc_ejected, g_harvesting);
        if (g_sd_error[0]) cdc_printf("CLI: sd_error=%s\r\n", g_sd_error);
        if (g_harvest_log[0]) { cdc_printf("%s", g_harvest_log); g_harvest_log[0] = 0; }
        nvs_handle_t h;
        if (nvs_open("s3", NVS_READONLY, &h) == ESP_OK) {
            char apiHost[128] = "(none)", devId[16] = "(none)";
            nvs_get_string(h, "api_host", apiHost, sizeof(apiHost));
            nvs_get_string(h, "device_id", devId, sizeof(devId));
            cdc_printf("CLI: s3 api_host=%s device=%s\r\n", apiHost, devId);
            nvs_close(h);
        }
        cdc_printf("CLI: fw=%s usb=%s\r\n", FW_VERSION, g_msc_only ? "MSC-only" : "CDC+MSC");
        const esp_partition_t* running = esp_ota_get_running_partition();
        if (running) cdc_printf("CLI: ota partition=%s\r\n", running->label);

    } else if (strcmp(cmd, "RESETBOOT") == 0) {
        nvs_handle_t h;
        if (nvs_open("dbg", NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u32(h, "boots", 0);
            nvs_commit(h);
            nvs_close(h);
        }
        cdc_printf("CLI: boot counter reset\r\n");

    } else if (strcmp(cmd, "UPLOAD") == 0) {
        if (g_upload_task) {
            cdc_printf("CLI: triggering upload task\r\n");
            xTaskNotifyGive(g_upload_task);
        }

    } else if (strcmp(cmd, "FORMAT") == 0) {
        cdc_printf("CLI: formatting SD as 8GB FAT32 — ALL DATA WILL BE LOST\r\n");
        g_harvesting = true;
        g_msc_ejected = true;
        vTaskDelay(pdMS_TO_TICKS(500));

        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        // Unmount FATFS fully
        if (g_fatfs_mounted) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT, g_card);
            g_fatfs_mounted = false;
        }

        // Re-init SPI device + card for raw access
        int card_handle = -1;
        sdspi_host_init_device(&g_slot_config, &card_handle);
        g_sd_host.slot = card_handle;
        g_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
        bool fmt_ok = false;
        if (g_card && sdmmc_card_init(&g_sd_host, g_card) == ESP_OK) {
            g_card_sectors = g_card->csd.capacity;

            // Register diskio so f_fdisk/f_mkfs can access the card
            BYTE pdrv = 0xFF;
            ff_diskio_get_drive(&pdrv);
            if (pdrv != 0xFF) {
                ff_diskio_register_sdmmc(pdrv, g_card);
                char drv[3] = {(char)('0' + pdrv), ':', 0};

                // Partition: cap at 8 GB for avionics compatibility
                // f_fdisk plist values: percentage (1-100) or absolute sector count (>100)
                uint32_t fmt_sectors = msc_visible_sectors();
                cdc_printf("CLI: fdisk %lu sectors (%.1f GB of %.1f GB card)\r\n",
                           (unsigned long)fmt_sectors,
                           fmt_sectors * 512.0 / 1e9,
                           g_card_sectors * 512.0 / 1e9);

                LBA_t plist[] = {(LBA_t)fmt_sectors, 0, 0, 0};
                void *work = malloc(4096);
                if (work) {
                    FRESULT fr = f_fdisk(pdrv, plist, work);
                    if (fr == FR_OK) {
                        cdc_printf("CLI: fdisk OK, formatting FAT32...\r\n");
                        MKFS_PARM opt = {};
                        opt.fmt = FM_FAT32;
                        opt.n_fat = 2;
                        opt.au_size = 16 * 1024;
                        fr = f_mkfs(drv, &opt, work, 4096);
                        if (fr == FR_OK) {
                            cdc_printf("CLI: mkfs OK\r\n");
                            fmt_ok = true;
                        } else {
                            cdc_printf("CLI: mkfs FAILED (%d)\r\n", fr);
                        }
                    } else {
                        cdc_printf("CLI: fdisk FAILED (%d)\r\n", fr);
                    }
                    free(work);
                }
                ff_diskio_unregister(pdrv);
            }
            // Clean up raw SPI device so sdspi_mount can re-add it
            sdspi_host_remove_device(card_handle);
            free(g_card); g_card = nullptr;
        }

        // Remount via standard path
        if (fmt_ok) {
            esp_vfs_fat_mount_config_t mount_cfg = {};
            mount_cfg.format_if_mount_failed = false;
            mount_cfg.max_files = 5;
            mount_cfg.allocation_unit_size = 16 * 1024;
            esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &g_sd_host,
                                                      &g_slot_config, &mount_cfg, &g_card);
            if (ret == ESP_OK) {
                g_fatfs_mounted = true;
                g_card_sectors = g_card->csd.capacity;
                g_sd_ready = true;
                // Create harvested directory
                mkdir("/sdcard/harvested", 0775);
                cdc_printf("CLI: format complete, 8GB FAT32 ready\r\n");
            } else {
                cdc_printf("CLI: remount FAILED: %s\r\n", esp_err_to_name(ret));
            }
        }
        xSemaphoreGive(g_sd_mutex);

        g_harvesting = false;
        g_msc_ejected = false;

    } else if (strncmp(cmd, "SETMODE", 7) == 0) {
        const char* mode = cmd + 7;
        while (*mode == ' ') mode++;
        if (!mode[0]) {
            cdc_printf("CLI: current=%s  usage: SETMODE CDC | SETMODE MSC\r\n",
                       g_msc_only ? "MSC" : "CDC");
        } else {
            uint8_t val = 1;
            if (strcasecmp(mode, "CDC") == 0) val = 0;
            else if (strcasecmp(mode, "MSC") == 0) val = 1;
            else { cdc_printf("CLI: unknown mode '%s' — use CDC or MSC\r\n", mode); return; }
            nvs_handle_t h;
            if (nvs_open("usb", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "msc_only", val);
                nvs_commit(h);
                nvs_close(h);
            }
            cdc_printf("CLI: USB mode set to %s — reboot to apply\r\n", val ? "MSC-only" : "CDC+MSC");
        }

    } else if (strcmp(cmd, "OTA") == 0) {
        cdc_printf("CLI: checking for firmware update (current=%s)...\r\n", FW_VERSION);
        xTaskCreatePinnedToCore([](void*) {
            int result = otaCheck();
            g_tlsActive = false;
            if (result == 1) {
                cdc_printf("OTA: downloaded — rebooting in 3s\r\n");
                vTaskDelay(pdMS_TO_TICKS(10000));
                esp_restart();
            } else if (result == 0) {
                cdc_printf("OTA: up to date\r\n");
            } else {
                cdc_printf("OTA: check failed (network error)\r\n");
            }
            vTaskDelete(nullptr);
        }, "ota", 16384, nullptr, 2, nullptr, 1);

    } else if (strcmp(cmd, "REBOOT") == 0) {
        cdc_printf("CLI: rebooting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();

    } else if (strcmp(cmd, "MODEM") == 0) {
        cdc_printf("CLI: modem=%s ppp=%s rssi=%d op=%s\r\n",
            g_modemReady ? "ready" : "none",
            g_pppConnected ? "connected" : "disconnected",
            g_modemRssi, g_modemOp);

    } else if (strcmp(cmd, "MODEM_START") == 0) {
        if (!g_modem_task) {
            xTaskCreatePinnedToCore(modemTask, "modem", 16384, nullptr, 2,
                                    &g_modem_task, 0);
            cdc_printf("CLI: modem task started\r\n");
        } else {
            cdc_printf("CLI: modem task already running\r\n");
        }

    } else if (strcmp(cmd, "CELLTEST") == 0) {
        if (!g_pppConnected) {
            cdc_printf("CLI: PPP not connected — run MODEM_START first\r\n");
        } else if (!s3LoadCreds()) {
            cdc_printf("CLI: no S3 credentials — run SETS3 first\r\n");
        } else {
            cdc_printf("CLI: launching 10 MB cellular upload speed test...\r\n");
            xTaskCreatePinnedToCore([](void*) {
                const uint32_t TEST_SIZE = 10UL * 1024 * 1024;
                const char* testName = "celltest_10mb.bin";

                // Create 10 MB test file on SD
                cdc_printf("CellTest: creating %lu byte test file...\r\n", (unsigned long)TEST_SIZE);
                char filepath[80];
                snprintf(filepath, sizeof(filepath), "%s/harvested/%s", SD_MOUNT, testName);

                xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                FILE* f = fopen(filepath, "wb");
                if (f) {
                    static uint8_t block[4096];
                    memset(block, 0xAA, sizeof(block));
                    uint32_t remaining = TEST_SIZE;
                    while (remaining > 0) {
                        uint32_t toWrite = (remaining < sizeof(block)) ? remaining : sizeof(block);
                        fwrite(block, 1, toWrite, f);
                        remaining -= toWrite;
                    }
                    fclose(f);
                }
                xSemaphoreGive(g_sd_mutex);

                if (!f) {
                    cdc_printf("CellTest: failed to create test file\r\n");
                    vTaskDelete(nullptr); return;
                }

                // Upload using existing S3 multipart infrastructure
                uint32_t startMs = millis();
                bool ok = s3UploadFile(testName);
                uint32_t elapsed = millis() - startMs;
                float kbps = (elapsed > 0) ? (TEST_SIZE / 1024.0f) / (elapsed / 1000.0f) : 0;

                if (ok) {
                    cdc_printf("CellTest: SUCCESS! %lu bytes in %.1fs = %.1f KB/s\r\n",
                        (unsigned long)TEST_SIZE, elapsed / 1000.0f, kbps);
                } else {
                    cdc_printf("CellTest: FAILED after %.1fs\r\n", elapsed / 1000.0f);
                }

                // Clean up test file and marker
                remove(filepath);
                char marker[96];
                snprintf(marker, sizeof(marker), "%s/harvested/.done__%s", SD_MOUNT, testName);
                remove(marker);

                s3ClearSession();
                vTaskDelete(nullptr);
            }, "celltest", 16384, nullptr, 3, nullptr, 1);
        }

    } else {
        cdc_printf("CLI: unknown: '%s'\r\n", cmd);
        cdc_printf("CLI: commands: SETWIFI SETS3 STATUS UPLOAD FORMAT REBOOT MODEM MODEM_START CELLTEST\r\n");
    }
}

// ── Main loop task ──────────────────────────────────────────────────────────
static void main_loop_task(void* param) {
    (void)param;

    // Delayed USB presentation — aircraft DSU needs device to appear after boot
    #define USB_PRESENT_DELAY_MS 60000
    uint32_t usbPresentMs = millis();

    for (;;) {
        // Watchdog: restart modem task if it died (init failure OR runtime crash)
        if (g_modem_task == nullptr) {
            log_write("Modem: task died — restarting");
            cdc_printf("Modem: restarting task...\r\n");
            g_modemReady = false;
            g_pppConnected = false;
            xTaskCreatePinnedToCore(modemTask, "modem", 16384, nullptr, 2, &g_modem_task, 0);
            vTaskDelay(pdMS_TO_TICKS(30000));  // 30s cooldown for modem cold boot
        }

        // Enable USB MSC after delay (card sectors must be valid)
        if (!g_sd_ready && g_card_sectors > 0 && (millis() - usbPresentMs) >= USB_PRESENT_DELAY_MS) {
            g_sd_ready = true;
            log_write("USB: drive presented to host (after %ds delay)", USB_PRESENT_DELAY_MS / 1000);
            cdc_printf("USB: drive ready\r\n");
        }

        // Print buffered harvest log
        if (g_harvest_log[0]) {
            ESP_LOGI(TAG, "%s", g_harvest_log);
            g_harvest_log[0] = '\0';
        }

        uint32_t now = millis();
        if (!g_splashActive && !g_otaActive && now - g_lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            g_lastDisplayMs = now;

            // ── Compute live speeds from deltas ──────────────────────────
            {
                static float    prevUsbMb = 0;
                static float    prevUpMb  = 0;
                static uint32_t prevMs    = 0;
                float dt = (prevMs > 0) ? (now - prevMs) / 1000.0f : 1.0f;
                if (dt > 0.1f) {
                    float usbDelta = g_hostWrittenMb - prevUsbMb;
                    float upDelta  = (g_mbUploaded + g_uploadingMb) - prevUpMb;
                    g_usbWriteKBps = (usbDelta > 0) ? (usbDelta * 1024.0f / dt) : 0;
                    g_uploadKBps   = (upDelta > 0)  ? (upDelta  * 1024.0f / dt) : 0;
                    prevUsbMb = g_hostWrittenMb;
                    prevUpMb  = g_mbUploaded + g_uploadingMb;
                    prevMs    = now;
                }
            }

            // Update SD used space (only when MSC is ejected to avoid SPI conflict)
            if (g_fatfs_mounted && g_msc_ejected &&
                xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                FATFS* fs;
                DWORD freeClusters;
                if (f_getfree("0:", &freeClusters, &fs) == FR_OK) {
                    DWORD totalSectors = (fs->n_fatent - 2) * fs->csize;
                    DWORD freeSectors  = freeClusters * fs->csize;
                    g_sdUsedMb = (totalSectors - freeSectors) * 512.0f / 1e6f;
                }
                xSemaphoreGive(g_sd_mutex);
            }
            doUpdateDisplay();
        }

        // Snapshot volatile write timestamp to avoid race with MSC callback
        uint32_t lastWr = g_lastWriteMs;
        if (!g_harvesting && g_writeDetected && g_hostWasConnected &&
            lastWr != 0 && now >= lastWr && (now - lastWr) >= QUIET_WINDOW_MS &&
            (now - g_lastHarvestMs) >= g_harvestCoolMs) {
            cdc_printf("Harvest: %.1f KB written, %us idle\r\n",
                     g_hostWrittenMb * 1024.0f, (now - lastWr) / 1000);
            log_write("Harvest trigger: %.1fKB, %us idle", g_hostWrittenMb * 1024.0f, (now - lastWr) / 1000);
            if (g_harvest_task) xTaskNotifyGive(g_harvest_task);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ── app_main ────────────────────────────────────────────────────────────────
extern "C" void app_main(void) {
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    log_init();

    // ── HAL initialization ─────────────────────────────────────────────
    static Esp32Display  s_display;
    static Esp32Clock    s_clock;
    static Esp32Nvs      s_nvs;
    static Esp32Filesys  s_filesys;
    static Esp32Network  s_network;
    static HAL           s_hal = { &s_display, &s_clock, &s_nvs, &s_filesys, &s_network };
    g_hal = &s_hal;

    g_sd_mutex = xSemaphoreCreateMutex();

    // ── Crash-loop detection ────────────────────────────────────────────
    {
        nvs_handle_t h;
        if (nvs_open("dbg", NVS_READWRITE, &h) == ESP_OK) {
            uint32_t boots = 0;
            nvs_get_u32(h, "boots", &boots);
            boots++;
            nvs_set_u32(h, "boots", boots);
            nvs_commit(h);
            nvs_close(h);
            g_bootCount = boots;

            esp_reset_reason_t reason = esp_reset_reason();
            ESP_LOGI(TAG, "Boot #%u  reset_reason=%d  heap=%lu",
                     boots, (int)reason, (unsigned long)esp_get_free_heap_size());

            if (boots > 5 && reason != ESP_RST_POWERON) {
                ESP_LOGW(TAG, "CRASH LOOP DETECTED — pausing 30s for debug");
                g_hal->display->init();
                disp("CRASH LOOP", "Paused 30s");

                // OTA rollback: if last OTA is pending, revert to previous partition
                nvs_handle_t hota;
                if (nvs_open("ota", NVS_READWRITE, &hota) == ESP_OK) {
                    char status[16] = "";
                    size_t slen = sizeof(status);
                    if (nvs_get_str(hota, "ota_status", status, &slen) == ESP_OK
                        && strcmp(status, "pending") == 0) {
                        ESP_LOGW(TAG, "OTA rollback: reverting to previous partition");
                        disp("CRASH LOOP", "OTA rollback...");
                        const esp_partition_t* prev = esp_ota_get_next_update_partition(NULL);
                        if (prev) {
                            esp_ota_set_boot_partition(prev);
                            nvs_set_str(hota, "ota_status", "rolled_back");
                            nvs_commit(hota);
                        }
                    }
                    nvs_close(hota);
                }

                vTaskDelay(pdMS_TO_TICKS(30000));
                nvs_handle_t h2;
                if (nvs_open("dbg", NVS_READWRITE, &h2) == ESP_OK) {
                    nvs_set_u32(h2, "boots", 0);
                    nvs_commit(h2);
                    nvs_close(h2);
                }
            } else {
                // Normal boot — confirm OTA success + reset counter
                nvs_handle_t h2;
                if (nvs_open("dbg", NVS_READWRITE, &h2) == ESP_OK) {
                    nvs_set_u32(h2, "boots", 0);
                    nvs_commit(h2);
                    nvs_close(h2);
                }
                nvs_handle_t hota;
                if (nvs_open("ota", NVS_READWRITE, &hota) == ESP_OK) {
                    nvs_set_str(hota, "fw_ver", FW_VERSION);
                    nvs_set_str(hota, "ota_status", "ok");
                    nvs_commit(hota);
                    nvs_close(hota);
                }
            }
        }
    }

    // ── MSC-only mode selection (NVS-based) ────────────────────────────
    {
        nvs_handle_t h;
        if (nvs_open("usb", NVS_READWRITE, &h) == ESP_OK) {
            uint8_t mode = 0;  // default: CDC+MSC (forced for debug) until SETMODE MSC is run
            nvs_get_u8(h, "msc_only", &mode);
            g_msc_only = (mode != 0);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "USB mode: %s", g_msc_only ? "MSC-only" : "CDC+MSC");
    }

    // ── OLED init ───────────────────────────────────────────────────────
    if (!g_hal->display->ok()) g_hal->display->init();
    if (!g_hal->display->ok()) {
        ESP_LOGE(TAG, "SSD1306 failed — continuing without display");
    }

    // ── Provision S3 upload credentials on first boot ───────────────────
    {
        nvs_handle_t h;
        if (nvs_open("s3", NVS_READWRITE, &h) == ESP_OK) {
            char tmp[4] = "";
            size_t len = sizeof(tmp);
            if (nvs_get_str(h, "api_host", tmp, &len) != ESP_OK || tmp[0] == '\0') {
                nvs_set_str(h, "api_host", "disw6oxjed.execute-api.us-west-2.amazonaws.com");
                nvs_set_str(h, "api_key",  "7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm");
                nvs_commit(h);
                ESP_LOGI(TAG, "S3 upload credentials provisioned");
            }
            // Auto-generate device_id from MAC
            len = sizeof(tmp);
            if (nvs_get_str(h, "device_id", tmp, &len) != ESP_OK || tmp[0] == '\0') {
                uint8_t mac[6];
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                char macStr[16];
                snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                nvs_set_str(h, "device_id", macStr);
                nvs_commit(h);
                ESP_LOGI(TAG, "Device ID: %s", macStr);
            }
            nvs_close(h);
        }
    }

    disp("Init SD...");

    // ── SD card init ────────────────────────────────────────────────────
    {
        bool has_sd = false;
        for (int i = 1; i <= 10 && !has_sd; i++) {
            has_sd = sd_init();
            if (!has_sd) {
                ESP_LOGE(TAG, "SD init attempt %d failed", i);
                disp("SD init...", i <= 5 ? "retrying" : "check card");
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
        if (!has_sd) {
            ESP_LOGE(TAG, "SD init failed — NOT formatting (data preservation)");
            ESP_LOGI(TAG, "Use CLI command FORMAT to format if card is truly blank");
            disp("SD FAILED", "serial: FORMAT");
        }
    }

    g_sdTotalMb = g_card_sectors * 512.0f / 1e6f;

    // Initialize SD used space for display
    if (g_fatfs_mounted) {
        FATFS* fs;
        DWORD freeClusters;
        if (f_getfree("0:", &freeClusters, &fs) == FR_OK) {
            DWORD totalSectors = (fs->n_fatent - 2) * fs->csize;
            DWORD freeSectors  = freeClusters * fs->csize;
            g_sdUsedMb = (totalSectors - freeSectors) * 512.0f / 1e6f;
        }
    }

    // Boot splash is shown later, after file scan

    // FATFS already mounted by sd_init() — no separate mount needed

    // ── TinyUSB init ────────────────────────────────────────────────────
    {
        // MSC-only descriptors: pure mass storage device (no CDC/IAD)
        // Uses different PID (0x0002) so OS doesn't load cached composite driver
        static const tusb_desc_device_t msc_only_dev_desc = {
            .bLength            = sizeof(tusb_desc_device_t),
            .bDescriptorType    = TUSB_DESC_DEVICE,
            .bcdUSB             = 0x0200,
            .bDeviceClass       = 0x00,
            .bDeviceSubClass    = 0x00,
            .bDeviceProtocol    = 0x00,
            .bMaxPacketSize0    = 64,
            .idVendor           = 0x1209,
            .idProduct          = 0x0002,
            .bcdDevice          = 0x0100,
            .iManufacturer      = 0x01,
            .iProduct           = 0x02,
            .iSerialNumber      = 0x03,
            .bNumConfigurations = 0x01,
        };
        #define MSC_ONLY_CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
        static const uint8_t msc_only_cfg_desc[] = {
            TUD_CONFIG_DESCRIPTOR(1, 1, 0, MSC_ONLY_CFG_LEN,
                                  TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
            TUD_MSC_DESCRIPTOR(0, 4, 0x01, 0x81, 64),
        };
        static const char msc_only_langid[] = {0x09, 0x04};
        static const char* msc_only_str_desc[] = {
            msc_only_langid,       // 0: English
            "AirBridge",           // 1: Manufacturer
            "USB Storage",         // 2: Product
            "AB0001",              // 3: Serial
            "Mass Storage",        // 4: MSC Interface
        };

        tinyusb_config_t tusb_cfg = {};
        tusb_cfg.external_phy = false;
        tusb_cfg.self_powered = false;
        tusb_cfg.vbus_monitor_io = -1;
        if (g_msc_only) {
            tusb_cfg.device_descriptor        = &msc_only_dev_desc;
            tusb_cfg.configuration_descriptor = msc_only_cfg_desc;
            tusb_cfg.string_descriptor        = msc_only_str_desc;
            tusb_cfg.string_descriptor_count  = 5;
        }
        ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

        if (!g_msc_only) {
            tinyusb_config_cdcacm_t cdc_cfg = {};
            cdc_cfg.usb_dev = TINYUSB_USBDEV_0;
            cdc_cfg.cdc_port = TINYUSB_CDC_ACM_0;
            cdc_cfg.callback_rx = cdc_rx_callback;
            cdc_cfg.callback_line_coding_changed = cdc_line_coding_callback;
            ESP_ERROR_CHECK(tusb_cdc_acm_init(&cdc_cfg));
        }

        if (g_card_sectors > 0) {
            // Delay USB presentation to host by 60s — aircraft DSU needs
            // the device to appear after boot, not during boot.
            // g_sd_ready stays false; a timer in main_loop_task enables it.
            g_sd_ready = false;
            uint32_t vis = msc_visible_sectors();
            ESP_LOGI(TAG, "MSC ready: %lu visible sectors (%.0f MB), card=%lu sectors (%.0f MB)",
                     (unsigned long)vis, vis * 512.0f / 1e6f,
                     (unsigned long)g_card_sectors, g_sdTotalMb);
            disp("USB drive ready", "");
        } else {
            disp("No SD card", "CLI: FORMAT");
        }
    }

    g_lastDisplayMs = millis();

    // ── WiFi disabled — cellular only ────────────────────────────────────
    // wifi_init();
    // Initialize event loop + netif (needed for PPP even without WiFi)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ── Create tasks ────────────────────────────────────────────────────
    xTaskCreatePinnedToCore(uploadTask,    "upload",    16384, nullptr, 1, &g_upload_task,  1);
    xTaskCreatePinnedToCore(harvestTask,   "harvest",   16384, nullptr, 1, &g_harvest_task, 1);
    xTaskCreatePinnedToCore(modemTask,     "modem",     16384, nullptr, 2, &g_modem_task,   0);  // core 0, 16KB stack for reconnection
    xTaskCreatePinnedToCore(main_loop_task, "main_loop", 4096, nullptr, 1, nullptr,         0);

    // ── Scan /harvested/ for leftover files from before last reboot ─────
    if (g_fatfs_mounted) {
        char dirpath[64];
        snprintf(dirpath, sizeof(dirpath), "%s/harvested", SD_MOUNT);
        DIR* dir = opendir(dirpath);
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (ent->d_type == DT_DIR) continue;
                if (ent->d_name[0] == '.') continue;
                char fullpath[128];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", dirpath, ent->d_name);
                struct stat st;
                if (stat(fullpath, &st) == 0 && st.st_size > 0) {
                    g_filesQueued++;
                    g_mbQueued += (float)st.st_size / 1e6f;
                }
            }
            closedir(dir);
            if (g_filesQueued > 0) {
                ESP_LOGI(TAG, "Boot: found %u file(s) in /harvested/ — notifying upload", g_filesQueued);
                xTaskNotifyGive(g_upload_task);
            }
        }
    }

    // ── Scan SD root for unharvested files (from previous session) ─────
    // Use g_fatfs_mounted not g_sd_ready (USB delayed 60s, but SD is ready)
    if (g_fatfs_mounted && g_filesQueued == 0) {
        DIR* rootDir = opendir(SD_MOUNT);
        if (rootDir) {
            bool found = false;
            struct dirent* ent;
            while ((ent = readdir(rootDir)) != nullptr) {
                if (ent->d_type == DT_DIR) continue;
                if (ent->d_name[0] == '.') continue;
                if (strcmp(ent->d_name, "airbridge.log") == 0) continue;
                // Check if already uploaded (.done marker exists)
                char donePath[192];
                snprintf(donePath, sizeof(donePath), "%s/harvested/.done__%s",
                         SD_MOUNT, ent->d_name);
                struct stat ds;
                if (stat(donePath, &ds) == 0) continue;  // already uploaded
                found = true;
                ESP_LOGI(TAG, "Boot: unharvested file in root: %s", ent->d_name);
                break;
            }
            closedir(rootDir);
            if (found) {
                ESP_LOGI(TAG, "Boot: triggering harvest for unharvested root files");
                g_writeDetected = true;
                g_hostWasConnected = true;
                g_lastWriteMs = millis();
            }
        }
    }

    // ── Boot splash (10s) ──────────────────────────────────────────────
    if (g_hal->display->ok()) {
        // Load device ID for display
        s3LoadCreds();

        oled_clear();
        // "AirBridge" 2x: 9 chars * 12px = 108px, center: (128-108)/2 = 10
        oled_text(10, 0, "AirBridge", 2);

        // SD capacity
        char sdLine[22];
        char usedStr[10], totalStr[10];
        _fmtSize(usedStr, sizeof(usedStr), g_sdUsedMb);
        float visMb = msc_visible_sectors() * 512.0f / 1e6f;
        _fmtSize(totalStr, sizeof(totalStr), visMb);
        snprintf(sdLine, sizeof(sdLine), "SD %s / %s", usedStr, totalStr);
        int sdW = strlen(sdLine) * 6;
        oled_text((128 - sdW) / 2, 22, sdLine);

        // Pending uploads
        if (g_filesQueued > 0) {
            char pendLine[22];
            char qStr[10];
            _fmtSize(qStr, sizeof(qStr), g_mbQueued);
            snprintf(pendLine, sizeof(pendLine), "%u file(s) %s queued", g_filesQueued, qStr);
            int pW = strlen(pendLine) * 6;
            oled_text((128 - pW) / 2, 34, pendLine);
        }

        // Device ID
        if (g_deviceId[0]) {
            char idLine[22];
            snprintf(idLine, sizeof(idLine), "ID:%s", g_deviceId);
            int idW = strlen(idLine) * 6;
            oled_text((128 - idW) / 2, 46, idLine);
        }

        // USB mode + firmware version
        {
            char modeLine[28];
            snprintf(modeLine, sizeof(modeLine), "%s v%s",
                     g_msc_only ? "MSC" : "CDC+MSC", FW_VERSION);
            int mW = strlen(modeLine) * 6;
            oled_text((128 - mW) / 2, 56, modeLine);
        }

        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
    g_splashActive = false;
    log_write("Boot: SD=%.1f/%.1fMB queued=%u heap=%lu",
              g_sdUsedMb, g_sdTotalMb, g_filesQueued,
              (unsigned long)esp_get_free_heap_size());

    ESP_LOGI(TAG, "app_main done — free heap: %lu, min: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());

    // app_main returns; FreeRTOS continues running our tasks
}

// AirBridge ESP32-S3 Firmware — ESP-IDF native port
// USB Mass Storage + SD card harvest + S3 upload + WiFi + captive portal + OLED
//
// Build: cd esp32 && ~/.local/bin/pio run
// Flash: 1200-baud touch on CDC port, then pio run -t upload

#define FW_VERSION "20260629030000"

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
#include "hal/net_util.h"
#include "airbridge_display.h"
#include "airbridge_wifi_creds.h"
#include "airbridge_harvest.h"
#include "airbridge_http.h"
#include "airbridge_s3.h"
#include "airbridge_triggers.h"
#include "airbridge_cli.h"
#include "airbridge_commands.h"
#include "airbridge_modem.h"
#include "airbridge_runtime.h"
#include "airbridge_sd.h"
#include "airbridge_log.h"

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

// ── SD/SPI mutex ────────────────────────────────────────────────────────────
// Serializes SD access between harvest/upload tasks and the USB MSC callbacks.
// Declared early so the Esp32Filesys HAL impl (used by shared upload code) can
// take it around streaming reads.
static SemaphoreHandle_t g_sd_mutex = nullptr;
static volatile bool     g_sd_ready = false;
static volatile bool     g_tlsActive = false; // suppress +++ escape during TLS; lengthens modem-watchdog stale threshold

// ── MSC-only mode (no CDC) ──────────────────────────────────────────────────
// Default: MSC-only for avionics compatibility. Set via CLI: SETMODE CDC / SETMODE MSC
// Persists in NVS. CDC mode gives serial console for debugging + config.
static bool g_msc_only = true;   // default MSC-only; ENABLE_CDC on SD overrides per-boot

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
        // Populate is_dir from d_type (reliable on the ESP-IDF FATFS VFS). Shared
        // code (e.g. findNextUploadFile) relies on this; size still needs stat().
        entry->is_dir = (ent->d_type == DT_DIR);
        entry->size = 0;
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
    // SD is shared with the USB MSC callbacks — serialize streaming reads.
    void lock() override   { if (g_sd_mutex) xSemaphoreTake(g_sd_mutex, portMAX_DELAY); }
    void unlock() override { if (g_sd_mutex) xSemaphoreGive(g_sd_mutex); }
};

// Defined later — the live TLS connect/destroy (sets g_tlsActive, socket
// timeouts, detailed error logging). Esp32Network wraps them so the shared
// upload code (airbridge_s3.h) uses the exact same TLS path as the firmware.
static esp_tls_t* tls_connect(const char* host);
static void       tls_destroy(esp_tls_t* tls);

class Esp32Network : public INetwork {
public:
    TlsHandle connect(const char* host) override {
        return (TlsHandle)tls_connect(host);
    }
    // Non-blocking primitive: WANT_WRITE -> 0 (would-block), other errors -> -1. A dead
    // cellular link returns WANT_WRITE indefinitely (the non-blocking socket never trips
    // SO_SNDTIMEO); the shared netWriteAll() bounds it with a wall-clock no-progress timeout
    // so the upload fails and reconnect can run instead of the task wedging forever.
    int writeSome(TlsHandle conn, const void* data, size_t len) override {
        int w = esp_tls_conn_write((esp_tls_t*)conn, data, len);
        if (w > 0) return w;
        if (w == ESP_TLS_ERR_SSL_WANT_WRITE) return 0;
        return -1;
    }
    bool write(TlsHandle conn, const void* data, size_t len) override {
        return netWriteAll(this, conn, data, len);
    }
    int read(TlsHandle conn, void* buf, size_t len) override {
        return esp_tls_conn_read((esp_tls_t*)conn, buf, len);
    }
    void destroy(TlsHandle conn) override {
        tls_destroy((esp_tls_t*)conn);
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

// g_sd_mutex / g_sd_ready declared earlier (needed by the Esp32Filesys HAL impl).

// ── Harvest timing ──────────────────────────────────────────────────────────
// QUIET_WINDOW_MS defined in airbridge_triggers.h
#define DISPLAY_INTERVAL_MS  1000UL

static volatile bool     g_hostConnected    = false;
static volatile bool     g_hostWasConnected = false;
static volatile bool     g_bootHarvestPending = false; // set by boot scan; bypasses quiet window
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
static volatile bool g_preUsbDone   = false; // upload task signals OTA+cookie done → present USB
static float    g_uploadingMb    = 0.0f; // live progress of current file upload
// g_tlsActive declared earlier (needed by the Esp32Network HAL impl).
static float    g_uploadBaseMb   = 0.0f; // base offset for multipart (completed parts)
static float    g_usbWriteKBps   = 0.0f; // live USB write speed for display
static float    g_uploadKBps     = 0.0f; // live upload speed for display
// Connection quality from the data path (no AT/+++ needed). Bandwidth is set by
// real file/OTA transfers; the 60s log upload refreshes g_linkOkMs as a liveness
// heartbeat. Drives the OLED quality bars (replaces RSSI bars) + STATUS line.
// Connection quality from the data path (no AT/+++): the round-trip LATENCY of the
// small requests the device already makes (esp. the 60s log upload) tracks cellular
// quality without needing a big transfer to measure bandwidth. Drives the OLED
// quality bars (replaces RSSI bars). Bandwidth is shown separately during uploads.
static LinkWindow g_linkLatWin   = {};   // recent request RTT samples (ms)
static uint32_t g_linkOkMs       = 0;    // millis() of last successful request (liveness)
#define LINK_LAT_WINDOW_MS 180000        // best RTT over the last ~3 min (steady bars)
static inline void noteLinkLatency(uint32_t rttMs) {
    g_linkOkMs = millis();
    if (rttMs > 0) linkWindowAdd(g_linkLatWin, g_linkOkMs, (float)rttMs);
}
// Windowed-best (min) RTT for the display + STATUS line; <0 if no recent sample.
static inline float linkLatencyNow() { return linkWindowMin(g_linkLatWin, millis(), LINK_LAT_WINDOW_MS); }
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

// ── Dual-partition SD layout ────────────────────────────────────────────────
// Partition 1 (8 GB): DSU-facing, presented via MSC raw sectors
// Partition 2 (rest): firmware internal (upload/, logs/)
static bool     g_dual_partition   = false;
static bool     g_p2_needs_format  = false; // deferred: format P2 only in upload task
static bool     g_needs_full_format = false; // deferred: full repartition in upload task
// Runtime SD-health recovery (see sdHealthUpdate/sdRecoveryAction in airbridge_runtime.h)
static volatile bool g_sd_degraded = false; // card declared unusable at runtime (drives OLED + cellular log egress)
static volatile bool g_sd_reformatting = false; // destructive reformat scheduled (OLED)
static uint32_t g_p2_start_sector  = 0;  // partition 2 LBA start
static uint32_t g_p2_sectors       = 0;  // partition 2 size in sectors
static FATFS*   g_p2_fs            = nullptr; // partition 2 FATFS object
static const char* DSU_MOUNT       = "/dsu";  // temp mount point for partition 1

// Custom diskio drivers for P1 and P2: each offsets sector addresses by the
// partition's LBA start so f_mkfs/f_mount see a "disk" starting at the
// partition data, not at the raw MBR.  Using FM_FAT32|FM_SFD with these
// drivers places the FAT VBR directly at the partition start sector —
// no nested "sub-MBR" is created inside the partition data area.
static uint32_t g_p1_start_sector = 63;  // default; updated after f_fdisk

// ── P1 (DSU partition) diskio ─────────────────────────────────────────────
static DSTATUS p1_diskio_init(BYTE pdrv) { (void)pdrv; return 0; }
static DSTATUS p1_diskio_status(BYTE pdrv) { (void)pdrv; return 0; }
static DRESULT p1_diskio_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count) {
    (void)pdrv;
    return sdmmc_read_sectors(g_card, buff, sector + g_p1_start_sector, count)
           == ESP_OK ? RES_OK : RES_ERROR;
}
static DRESULT p1_diskio_write(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count) {
    (void)pdrv;
    return sdmmc_write_sectors(g_card, (void*)buff, sector + g_p1_start_sector, count)
           == ESP_OK ? RES_OK : RES_ERROR;
}
static DRESULT p1_diskio_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(DWORD*)buff = MSC_MAX_SECTORS; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD*)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD*)buff = 1; return RES_OK;
        default: return RES_PARERR;
    }
}
static const ff_diskio_impl_t g_p1_diskio_impl = {
    .init = p1_diskio_init, .status = p1_diskio_status,
    .read = p1_diskio_read, .write  = p1_diskio_write,
    .ioctl = p1_diskio_ioctl,
};

// ── P2 (firmware-internal) diskio ─────────────────────────────────────────
static DSTATUS p2_diskio_init(BYTE pdrv) { (void)pdrv; return 0; }
static DSTATUS p2_diskio_status(BYTE pdrv) { (void)pdrv; return 0; }

static DRESULT p2_diskio_read(BYTE pdrv, BYTE* buff, DWORD sector, UINT count) {
    (void)pdrv;
    esp_err_t err = sdmmc_read_sectors(g_card, buff, sector + g_p2_start_sector, count);
    return (err == ESP_OK) ? RES_OK : RES_ERROR;
}

static DRESULT p2_diskio_write(BYTE pdrv, const BYTE* buff, DWORD sector, UINT count) {
    (void)pdrv;
    esp_err_t err = sdmmc_write_sectors(g_card, (void*)buff, sector + g_p2_start_sector, count);
    return (err == ESP_OK) ? RES_OK : RES_ERROR;
}

static DRESULT p2_diskio_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    (void)pdrv;
    switch (cmd) {
        case CTRL_SYNC: return RES_OK;
        case GET_SECTOR_COUNT: *(DWORD*)buff = g_p2_sectors; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD*)buff = 512; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD*)buff = 1; return RES_OK;
        default: return RES_PARERR;
    }
}

static const ff_diskio_impl_t g_p2_diskio_impl = {
    .init   = p2_diskio_init,
    .status = p2_diskio_status,
    .read   = p2_diskio_read,
    .write  = p2_diskio_write,
    .ioctl  = p2_diskio_ioctl,
};

// Helper: read LE uint32 from byte array (MBR partition table parsing)
static inline uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Helper: temp-mount partition 1 at /dsu for harvest/cookie operations
static bool mount_dsu() {
    esp_vfs_fat_conf_t conf = {};
    conf.base_path = DSU_MOUNT;
    conf.fat_drive = "0:";
    conf.max_files = 3;
    FATFS* fs = nullptr;
    if (esp_vfs_fat_register_cfg(&conf, &fs) != ESP_OK) return false;
    if (f_mount(fs, "0:", 1) != FR_OK) {
        esp_vfs_fat_unregister_path(DSU_MOUNT);
        return false;
    }
    return true;
}

static void unmount_dsu() {
    f_mount(NULL, "0:", 0);
    esp_vfs_fat_unregister_path(DSU_MOUNT);
}

// Helper: write cookie binary to DSU partition (P1 if dual, /sdcard if single)
static bool write_cookie_to_dsu(const uint8_t* cookie, size_t len) {
    if (!g_dual_partition) {
        char path[64];
        snprintf(path, sizeof(path), "%s/dsuCookie.easdf", SD_MOUNT);
        FILE* cf = fopen(path, "wb");
        if (!cf) return false;
        fwrite(cookie, 1, len, cf);
        fclose(cf);
        return true;
    }
    if (!mount_dsu()) return false;
    char path[64];
    snprintf(path, sizeof(path), "%s/dsuCookie.easdf", DSU_MOUNT);
    FILE* cf = fopen(path, "wb");
    bool ok = false;
    if (cf) { fwrite(cookie, 1, len, cf); fclose(cf); ok = true; }
    unmount_dsu();
    return ok;
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
static int               g_modemRsrp    = MODEM_SIG_NA;  // LTE RSRP dBm (9999=N/A)
static int               g_modemRsrq    = MODEM_SIG_NA;  // LTE RSRQ dB
static int               g_modemSinr    = MODEM_SIG_NA;  // LTE SINR dB
static char              g_modemBand[16] = "";           // LTE band (CPSI)

// No-progress watchdog: main_loop stamps this each iteration; an independent
// watchdog task reboots if it goes stale (main loop wedged). See watchdog_task().
static volatile uint32_t g_mainLoopHeartbeat = 0;
#define WATCHDOG_STALL_MS  300000   // 5 min main-loop stall → force reboot
#define WATCHDOG_CHECK_MS   15000   // watchdog poll cadence

// Antenna signal-survey mode: set by the `survey` airbridge.cmd directive at boot.
// The modem task registers then loops sampling RSSI/RSRP/RSRQ/SINR (never dials
// PPP), so AT polling never collides with an upload. Read live over CDC serial.
static volatile bool     g_surveyMode = false;
static volatile int      g_surveyBand = 0;   // 0=no change, -1=restore auto, >0=lock LTE band N

// Gzip .eaofh files into the upload queue at harvest (~3x fewer bytes over cellular).
// Set by the `compress` airbridge.cmd directive. Default OFF: the S3 consumer must
// gunzip the .eaofh objects, so this is enabled per-deployment once that's in place.
// g_compress is now the shared inline global in airbridge_commands.h (toggled by the
// `compress` directive on either the USB or S3 command path, read by the harvest).
#define SURVEY_INTERVAL_MS 2000

// ── CDC CLI ─────────────────────────────────────────────────────────────────
// CLI removed — CDC serial is now a log-only output stream.
// Configuration via SD magic files: WIFI_CONFIG, S3_CONFIG, ENABLE_CDC, firmware.bin

// Persistent CDC+MSC magic file (E2E/dev only). Unlike one-shot ENABLE_CDC, this
// file is NOT deleted after processing and is re-checked every boot — its presence
// keeps CDC+MSC on; remove it to revert to production MSC-only. Honored only when
// built with -DALLOW_CDC_PERSIST so a production OTA build silently ignores it.
#define CDC_PERSIST_MAGIC "CDC_PERSIST"

// ── Unified logging (early, before any callbacks that use log_write/cdc_printf)
static void _cdc_serial_sink(const char* buf, int len) {
    if (g_msc_only) return;
    if (tud_cdc_connected()) {
        tud_cdc_write(buf, len);
        tud_cdc_write_flush();
    }
}
#define log_write   airbridge_log
#define log_init()  airbridge_log_init(_cdc_serial_sink, millis)
// Log flush writes to session-specific file: /sdcard/logs/boot_NNNN.log
static void log_flush_to_sd() {
    if (!g_logFileName[0]) return;
    char path[64];
    snprintf(path, sizeof(path), "/sdcard/logs/%s.log", g_logFileName);
    airbridge_log_flush(path);
}

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

// ── Forward declarations ────────────────────────────────────────────────────
static void doUpdateDisplay();
static void disp(const char* line1, const char* line2 = nullptr);
static bool sd_mount_fatfs();
static void sd_unmount_fatfs();
static void sd_before_restart();

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
        g_hostWasConnected = true;
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
}  // extern "C"

// (logging functions moved to early section above, before TinyUSB callbacks)

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

    // Card works. Check MBR for dual-partition layout before deciding mount strategy.
    g_card_sectors = tmp_card->csd.capacity;
    g_card = tmp_card; tmp_card = nullptr;
    // Keep card_handle — we'll use it for raw access

    // ── Detect partition layout and decide format action ──────────────
    // Decision tree:
    //   1. Valid dual-partition (P1 ≤ 8GB, P2 exists) → use as-is
    //   2. P1 is 8GB but no P2 → add P2 entry to MBR, format P2 only
    //   3. Anything else (wrong layout, blank, oversized P1) → full reformat
    //   4. Card < 12GB → single-partition fallback (no room for useful P2)
    #define MIN_DUAL_SECTORS ((uint32_t)(12ULL * 1024 * 1024 * 1024 / 512))  // 12GB minimum
    {
        uint8_t* mbr = (uint8_t*)malloc(512);
        bool mbr_valid = mbr && sdmmc_read_sectors(g_card, mbr, 0, 1) == ESP_OK
                         && mbr[510] == 0x55 && mbr[511] == 0xAA;

        if (mbr_valid) {
            uint32_t p1_start = le32(mbr + 0x1BE + 8);
            uint32_t p1_size  = le32(mbr + 0x1BE + 12);
            uint32_t p2_start = le32(mbr + 0x1CE + 8);
            uint32_t p2_size  = le32(mbr + 0x1CE + 12);
            bool p1_is_ours = (p1_size > 0 && p1_size <= MSC_MAX_SECTORS + 2048);

            if (p1_is_ours && p2_start > 0 && p2_size > 0 &&
                p2_start + p2_size <= g_card_sectors) {
                // Case 1: valid dual-partition
                g_p2_start_sector = p2_start;
                g_p2_sectors = p2_size;
                g_dual_partition = true;
                ESP_LOGI(TAG, "SD: dual partition OK — P2 start=%lu size=%lu",
                         (unsigned long)p2_start, (unsigned long)p2_size);
            } else if (p1_is_ours && g_card_sectors >= MIN_DUAL_SECTORS) {
                // Case 2: our P1 exists but no valid P2 — add P2 to MBR.
                // Also fix P1's partition type if it's not a FAT type (e.g. 0x07
                // written by mkfs.fat on Linux or by f_fdisk).  This prevents
                // FATFS mount failures in the single-partition fallback path and
                // ensures the MBR is self-consistent after migration.
                uint64_t p1_end = (uint64_t)p1_start + p1_size;
                uint32_t avail = (p1_end < g_card_sectors) ? (uint32_t)(g_card_sectors - p1_end) : 0;
                if (avail > 1024000) {
                    ESP_LOGW(TAG, "SD: P1 OK but no P2 — adding P2 to MBR");
                    // Fix P1 type if not already 0x0B/0x0C/0x0E (FAT32)
                    const int P1E = 0x1BE;
                    if (mbr[P1E+4] != 0x0B && mbr[P1E+4] != 0x0C && mbr[P1E+4] != 0x0E) {
                        mbr[P1E+4] = 0x0C;  // FAT32 LBA
                        ESP_LOGW(TAG, "SD: fixed P1 partition type 0x%02x→0x0C", mbr[P1E+4]);
                    }
                    uint32_t p2s = (uint32_t)p1_end;
                    const int P2E = 0x1CE;
                    mbr[P2E] = 0x00;
                    mbr[P2E+1] = 0xFE; mbr[P2E+2] = 0xFF; mbr[P2E+3] = 0xFF;
                    mbr[P2E+4] = 0x0C;
                    mbr[P2E+5] = 0xFE; mbr[P2E+6] = 0xFF; mbr[P2E+7] = 0xFF;
                    mbr[P2E+8]  = p2s & 0xFF; mbr[P2E+9]  = (p2s>>8) & 0xFF;
                    mbr[P2E+10] = (p2s>>16) & 0xFF; mbr[P2E+11] = (p2s>>24) & 0xFF;
                    mbr[P2E+12] = avail & 0xFF; mbr[P2E+13] = (avail>>8) & 0xFF;
                    mbr[P2E+14] = (avail>>16) & 0xFF; mbr[P2E+15] = (avail>>24) & 0xFF;
                    if (sdmmc_write_sectors(g_card, mbr, 0, 1) == ESP_OK) {
                        g_p2_start_sector = p2s;
                        g_p2_sectors = avail;
                        g_dual_partition = true;
                        g_p2_needs_format = true;
                        ESP_LOGI(TAG, "SD: MBR updated — P2 format deferred");
                    }
                }
            } else if (g_card_sectors >= MIN_DUAL_SECTORS) {
                // Case 3: wrong layout — full reformat needed
                ESP_LOGW(TAG, "SD: layout mismatch (P1=%luMB) — full reformat needed",
                         (unsigned long)(p1_size / 2048));
                g_needs_full_format = true;
            }
            // else: Case 4 — card too small, fall through to single-partition
        } else if (g_card_sectors >= MIN_DUAL_SECTORS) {
            // No valid MBR (blank card or corrupted) — full reformat
            ESP_LOGW(TAG, "SD: no valid MBR — full reformat needed");
            g_needs_full_format = true;
        }
        free(mbr);
    }

    if (g_dual_partition) {
        // ── Dual-partition: mount partition 2 at /sdcard ────────────────
        // pdrv 0: standard sdmmc (whole card) — for temp partition 1 access
        ff_diskio_register_sdmmc(0, g_card);
        // pdrv 1: offset diskio for partition 2
        ff_diskio_register(1, &g_p2_diskio_impl);

        // Mount partition 2 at /sdcard via VFS
        esp_vfs_fat_conf_t conf = {};
        conf.base_path = SD_MOUNT;
        conf.fat_drive = "1:";
        conf.max_files = 5;
        esp_err_t vfs_ret = esp_vfs_fat_register_cfg(&conf, &g_p2_fs);
        if (vfs_ret == ESP_OK && g_p2_fs) {
            FRESULT fr = f_mount(g_p2_fs, "1:", 1);
            if (fr == FR_OK) {
                g_fatfs_mounted = true;
                mkdir("/sdcard/upload", 0775);
                mkdir("/sdcard/logs", 0775);
                ESP_LOGI(TAG, "SD: partition 2 mounted at %s", SD_MOUNT);
            } else {
                // P2 exists in MBR but isn't formatted — defer format to upload task
                ESP_LOGW(TAG, "SD: P2 mount failed (FR=%d) — deferring format", fr);
                snprintf(g_sd_error, sizeof(g_sd_error), "P2 mount: FR=%d (will format)", fr);
                esp_vfs_fat_unregister_path(SD_MOUNT);
                g_p2_needs_format = true;
            }
        } else {
            snprintf(g_sd_error, sizeof(g_sd_error), "P2 vfs: %s", esp_err_to_name(vfs_ret));
        }
    } else {
        // ── Single partition (legacy): use esp_vfs_fat_sdspi_mount ─────
        // Need to clean up manual card init first
        free(g_card); g_card = nullptr;
        sdspi_host_remove_device(card_handle);

        esp_vfs_fat_mount_config_t mount_cfg = {};
        mount_cfg.format_if_mount_failed = false;
        mount_cfg.max_files = 5;
        mount_cfg.allocation_unit_size = 16 * 1024;

        ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &g_sd_host,
                                       &g_slot_config, &mount_cfg, &g_card);
        if (ret == ESP_OK) {
            g_fatfs_mounted = true;
            g_card_sectors = g_card->csd.capacity;
        } else {
            snprintf(g_sd_error, sizeof(g_sd_error), "mount: %s", esp_err_to_name(ret));
            sdspi_host_init_device(&g_slot_config, &card_handle);
            g_sd_host.slot = card_handle;
            g_card = (sdmmc_card_t *)calloc(1, sizeof(sdmmc_card_t));
            if (g_card) {
                sdmmc_card_init(&g_sd_host, g_card);
                g_card_sectors = g_card->csd.capacity;
            }
        }
    }

    ESP_LOGI(TAG, "SD: %lu sectors, fatfs=%s, dual=%d",
             (unsigned long)g_card_sectors, g_fatfs_mounted ? "ok" : "FAIL", g_dual_partition);
    airbridge_log("SD init: sectors=%lu fatfs=%s dual=%d err='%s'",
                  (unsigned long)g_card_sectors,
                  g_fatfs_mounted ? "ok" : "FAIL",
                  (int)g_dual_partition,
                  g_sd_error[0] ? g_sd_error : "none");
    return true;
}

static bool sd_mount_fatfs() {
    if (g_fatfs_mounted) return true;

    if (g_dual_partition && g_p2_fs) {
        // Partition 2: just remount the FATFS volume
        FRESULT fr = f_mount(g_p2_fs, "1:", 1);
        g_fatfs_mounted = (fr == FR_OK);
        return g_fatfs_mounted;
    }

    // Single partition (legacy)
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

    if (g_dual_partition) {
        f_mount(NULL, "1:", 0);
    } else {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, g_card);
    }
    g_fatfs_mounted = false;
}

// Re-initialize card + remount FATFS (used by doHarvest after MSC writes)
static bool sd_reinit_and_mount() {
    sd_unmount_fatfs();
    return sd_mount_fatfs();
}

// ── Unified command file (airbridge.cmd) ────────────────────────────────────
// One persistent text file of directives (see airbridge_commands.h). Processed
// at boot (all directives) and during harvest (runtimeOnly=true => only
// directives that don't need a reboot). `once` directives are stripped after
// running. Reboot/format are signalled back to the caller, which owns the SD
// teardown. logsDir/uploadDir are always on P2 (where logs live); diagDir is the
// USB-visible destination chosen by the caller.
// Thin firmware wrapper over the SHARED runCommandTextBuffer (airbridge_commands.h).
// The HAL-safe directives (dump_logs/wifi/s3/compress) run inside the shared core; this
// wrapper applies the firmware-/target-specific bits the core returns as flags
// (g_msc_only + the ALLOW_CDC_PERSIST gate, g_surveyMode/Band, the OLED) and persists the
// once-stripped rewrite. The S3 command channel calls runCommandTextBuffer directly
// (in-memory, SD-independent) so both delivery paths share identical execution.
static CmdRunResult run_command_file(const char* cmdPath, const char* diagDir,
                                     bool runtimeOnly) {
    // STATIC buffers, not stack: this runs in app_main on the small main task
    // (CONFIG_ESP_MAIN_TASK_STACK_SIZE=3584) via check_p1_magic; ~4.5KB of stack arrays
    // here overflowed it and hung the boot. Safe — check_p1_magic finishes before any
    // task starts, and the harvest task (the only other caller) never overlaps it.
    static char text[2048];
    static char out[2048];
    CmdRunResult res = {};
    if (!cmdReadFile(cmdPath, text, sizeof(text))) return res;

    res = runCommandTextBuffer(text, runtimeOnly, diagDir, "/sdcard/logs", "/sdcard/upload",
                               out, sizeof(out));

    // Target-specific glue the shared core deliberately leaves to firmware:
    if (res.cdc) {
        if (res.cdcOnce) {
            g_msc_only = false;
            airbridge_log("CMD: cdc once — CDC+MSC this boot");
            disp("USB Mode", "CDC (cmd)");
        } else {
#ifdef ALLOW_CDC_PERSIST
            g_msc_only = false;
            airbridge_log("CMD: cdc — CDC+MSC (persistent)");
            disp("USB Mode", "CDC (persist)");
#else
            airbridge_log("CMD: cdc (persistent) ignored in production build");
#endif
        }
    }
    if (res.survey) {
        g_surveyMode = true;
        g_surveyBand = res.surveyBand;   // 0=none, -1=auto, >0=lock LTE band N
        disp("Survey mode", "measuring signal");
    }

    // Persist the once-stripped rewrite (1=write remaining, 2=delete the empty file).
    if (res.rewrite == 1)      cmdWriteFile(cmdPath, out);
    else if (res.rewrite == 2 && g_hal && g_hal->filesys) g_hal->filesys->remove(cmdPath);
    return res;
}

// ── P1 magic files (dual-partition mode) ────────────────────────────────────
// In dual-partition mode, P2 (logs, config) is invisible to USB MSC.  This
// makes ENABLE_CDC on P2 unreachable when P2 FATFS is down — breaking the
// diagnostic catch-22.  Check P1 (USB-visible DSU partition) for the same
// magic files via a temporary mount_dsu().  Works regardless of P2 state.
//
// Usage: drop ENABLE_CDC or REBOOT on P1 via USB MSC; firmware picks it up
// on the next boot even if P2 FATFS is failing.
static void check_p1_magic() {
    if (!g_dual_partition || !g_card) return;
    if (!mount_dsu()) return;

    char path[64];

    // ENABLE_CDC on P1 — same one-boot CDC override as P2
    snprintf(path, sizeof(path), "%s/ENABLE_CDC", DSU_MOUNT);
    if (access(path, F_OK) == 0) {
        remove(path);
        g_msc_only = false;
        airbridge_log("P1: ENABLE_CDC — CDC+MSC for this boot");
        disp("USB Mode", "CDC (P1 flag)");
        vTaskDelay(pdMS_TO_TICKS(500));
    }

#ifdef ALLOW_CDC_PERSIST
    // CDC_PERSIST on P1 — persistent CDC+MSC, NOT removed (re-read every boot).
    snprintf(path, sizeof(path), "%s/%s", DSU_MOUNT, CDC_PERSIST_MAGIC);
    if (access(path, F_OK) == 0) {
        g_msc_only = false;
        airbridge_log("P1: CDC_PERSIST — CDC+MSC (persistent until removed)");
        disp("USB Mode", "CDC (persist)");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
#endif

    // Unified command file on P1 (airbridge.cmd). Diag dump dest is on P1 (USB-
    // visible). Runs all directives at boot; reboot/format handled below.
    {
        char cmdPath[80], diagDir[80];
        snprintf(cmdPath, sizeof(cmdPath), "%s/%s", DSU_MOUNT, COMMAND_FILE_NAME);
        snprintf(diagDir, sizeof(diagDir), "%s/diag", DSU_MOUNT);
        CmdRunResult cr = run_command_file(cmdPath, diagDir, /*runtimeOnly=*/false);
        if (cr.format) {
            airbridge_log("CMD: format_sd — full reformat on next boot");
            nvs_handle_t fh;
            if (nvs_open("sys", NVS_READWRITE, &fh) == ESP_OK) {
                nvs_set_u8(fh, "format", 1); nvs_commit(fh); nvs_close(fh);
            }
            unmount_dsu(); vTaskDelay(pdMS_TO_TICKS(500));
            sd_before_restart(); esp_restart();
        }
        if (cr.reboot) {
            airbridge_log("CMD: reboot — restarting");
            unmount_dsu(); vTaskDelay(pdMS_TO_TICKS(500));
            sd_before_restart(); esp_restart();
        }
    }

    // REBOOT on P1 — clean restart
    snprintf(path, sizeof(path), "%s/REBOOT", DSU_MOUNT);
    if (access(path, F_OK) == 0) {
        remove(path);
        airbridge_log("P1: REBOOT found — rebooting");
        unmount_dsu();
        vTaskDelay(pdMS_TO_TICKS(500));
        sd_before_restart();
        esp_restart();
    }

    // FORMAT_SD on P1 — set a persistent flag and reboot. The full reformat
    // (P1 + P2) runs in app_main after the next boot's sd_init (formatting at
    // runtime would need to tear down the active FATFS/MSC). Clears both
    // partitions, including a stuck/corrupt internal upload queue.
    snprintf(path, sizeof(path), "%s/FORMAT_SD", DSU_MOUNT);
    if (access(path, F_OK) == 0) {
        remove(path);
        airbridge_log("P1: FORMAT_SD found — full reformat on next boot");
        disp("Format SD", "rebooting...");
        nvs_handle_t fh;
        if (nvs_open("sys", NVS_READWRITE, &fh) == ESP_OK) {
            nvs_set_u8(fh, "format", 1); nvs_commit(fh); nvs_close(fh);
        }
        unmount_dsu();
        vTaskDelay(pdMS_TO_TICKS(500));
        sd_before_restart();
        esp_restart();
    }

    unmount_dsu();
}

// Call before esp_restart() to tear down SPI cleanly.
// Without this, soft reset (used by OTA) leaves the SPI DMA channel and GPIO
// matrix in a partially-initialized state. On next boot, spi_bus_initialize()
// may fail or the SD card won't respond, blocking FATFS for minutes.
static void sd_before_restart() {
    // Tear down FATFS and SPI so next boot initializes cleanly after soft reset.
    if (g_dual_partition) {
        // Dual-partition: manual teardown. ff_diskio and the SPI device are
        // registered independently; must remove both before spi_bus_free.
        if (g_fatfs_mounted) {
            f_mount(NULL, "1:", 0);
            esp_vfs_fat_unregister_path(SD_MOUNT);
        }
        ff_diskio_unregister(1);  // P2 offset diskio
        ff_diskio_unregister(0);  // raw card diskio (drive 0)
        // Remove the SPI device — required before spi_bus_free().
        // In dual-partition mode, g_sd_host.slot holds the handle from the
        // original sdspi_host_init_device() call and is never removed elsewhere.
        sdspi_host_remove_device((sdspi_dev_handle_t)g_sd_host.slot);
    } else if (g_fatfs_mounted) {
        // Single-partition: unmount handles VFS, diskio, and device removal.
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, g_card);
    }
    g_fatfs_mounted = false;
    // Free the SPI bus — resets DMA descriptors and GPIO matrix mux state.
    spi_bus_free(g_spi_host);
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

// ── WiFi / captive portal ─────────────────────────────────────────────────
// DISABLED on the active ESP-IDF+cellular branch (heap reserved for PPPoS+TLS).
// Credential logic lives in airbridge_wifi_creds.h; the Arduino fallback branch
// (esp32-s3) still uses WiFi. Removed dead wifi_init/wifiTask/captive-portal/DNS
// code from this file — see git history if the cellular variant ever needs WiFi.

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
    g_displayState.linkLatencyMs = linkLatencyNow();
    g_displayState.linkAgeMs     = g_linkOkMs ? (millis() - g_linkOkMs) : 0xFFFFFFFFu;
    // Card physically present but no filesystem mounted = SD fault — covers both
    // a boot-time mount failure and a runtime degrade, and auto-clears the moment
    // a (re)mount/reformat succeeds. (main_loop starts after sd_init + any boot
    // reformat, so this won't flash during the normal first mount.)
    g_displayState.sdError       = (g_card_sectors > 0 && !g_fatfs_mounted);
    g_displayState.sdReformatting = g_sd_reformatting;
    updateDisplay(g_displayState);
}

// ── S3 upload via pre-signed URLs ───────────────────────────────────────────
#define S3_CHUNK_SIZE (5UL * 1024 * 1024)

static char g_apiHost[128] = "";
static char g_apiKey[64]   = "";
static char g_deviceId[24] = "";  // 12-hex MAC id, or "TEST_<12hex>" (17) in e2e builds

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
    // Set 30s read/write timeout on the underlying socket
    int sock_fd = -1;
    if (esp_tls_get_conn_sockfd(tls, &sock_fd) == ESP_OK && sock_fd >= 0) {
        struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
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

// ── Aircraft manifest API ────────────────────────────────────────────────────
// GET /prod/aircraft/manifest?serial=X  → {high_water_mark: N, ...}
// Returns the hwm (0 on error or no manifest).
static uint32_t s3FetchManifest(const char* serial) {
    if (!s3LoadCreds()) return 0;
    esp_tls_t* tls = tls_connect(g_apiHost);
    if (!tls) return 0;
    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET /prod/aircraft/manifest?serial=%s HTTP/1.1\r\n"
        "Host: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
        serial, g_apiHost, g_apiKey);
    if (!tls_write_all(tls, req, rlen)) { tls_destroy(tls); return 0; }
    std::string resp = httpReadResponse(tls);
    tls_destroy(tls);
    int32_t hwm = jsonInt(resp, "\"high_water_mark\"");
    return (hwm > 0) ? (uint32_t)hwm : 0;
}

// POST /prod/aircraft/manifest with JSON body → update manifest + advance hwm.
static bool s3UpdateManifest(const char* serial, uint32_t firstFlight,
                             uint32_t lastFlight, const char* s3Key) {
    if (!s3LoadCreds()) return false;
    esp_tls_t* tls = tls_connect(g_apiHost);
    if (!tls) return false;
    char body[512];
    int bodyLen = snprintf(body, sizeof(body),
        "{\"serial\":\"%s\",\"last_flight\":%lu,\"first_flight\":%lu,\"s3_key\":\"%s\"}",
        serial, (unsigned long)lastFlight, (unsigned long)firstFlight, s3Key ? s3Key : "");
    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "POST /prod/aircraft/manifest HTTP/1.1\r\n"
        "Host: %s\r\nx-api-key: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n",
        g_apiHost, g_apiKey, bodyLen);
    bool ok = tls_write_all(tls, hdr, hlen) && tls_write_all(tls, body, bodyLen);
    if (ok) {
        std::string resp = httpReadResponse(tls);
        ok = resp.find("\"high_water_mark\"") != std::string::npos;
        if (!ok) cdc_printf("Manifest update failed: %.200s", resp.c_str());
    }
    tls_destroy(tls);
    return ok;
}

// Scan a .eaofh file to find the byte offset where flight (hwm+1) begins.
// Uses firstFlight/lastFlight from the .meta sidecar to estimate position,
// then scans a ±3-flight window. FILE* f must be open. fileSize = total bytes.
// Returns 0 when first_flight > hwm (all content is new; caller uploads from 0).
// Returns fileSize when all content is already covered (caller should skip).
static uint64_t findSplitOffset(FILE* f, uint64_t fileSize,
                                uint32_t firstFlight, uint32_t lastFlight,
                                uint32_t hwm) {
    if (firstFlight > hwm)   return 0;           // entirely new
    if (lastFlight  <= hwm)  return fileSize;     // entirely old

    uint32_t total = lastFlight - firstFlight + 1;
    uint32_t avg   = (total > 0) ? (uint32_t)(fileSize / total) : 0;
    if (avg == 0) return 0;

    // Estimate byte position of hwm's 0x4C record
    float frac    = (float)(hwm - firstFlight + 1) / (float)total;
    uint64_t est  = (uint64_t)(frac * (float)fileSize);
    uint64_t win  = (uint64_t)avg * 3;           // ±3 flight widths
    uint64_t scan_start = (est > win) ? (est - win) : 0;
    uint64_t scan_end   = est + win;
    if (scan_end > fileSize) scan_end = fileSize;

    // Buffered forward scan over the window
    const uint32_t CHUNK = 4096;
    uint8_t buf[CHUNK + 4];
    uint64_t pos   = scan_start;
    uint64_t hwm_block_end = 0;  // blockEnd of the hwm 0x4C (= start of hwm+1 block)
    uint32_t carry = 0;

    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    fseek(f, (long)scan_start, SEEK_SET);
    xSemaphoreGive(g_sd_mutex);

    while (pos < scan_end) {
        uint32_t want = (scan_end - pos > CHUNK) ? CHUNK : (uint32_t)(scan_end - pos);
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        uint32_t got = (uint32_t)fread(buf + carry, 1, want, f);
        xSemaphoreGive(g_sd_mutex);
        if (got == 0) break;
        uint32_t avail = carry + got;

        for (uint32_t i = 0; i + 3 < avail; i++) {
            if (buf[i] != 0xEA || buf[i+1] != 0x4C) continue;
            uint16_t rlen = ((uint16_t)buf[i+2] << 8) | buf[i+3];
            if (rlen < 28) continue;
            uint64_t recAbs = (pos - carry) + i;
            if (recAbs + rlen > fileSize) continue;

            // Read body[20:22] = flight BE u16
            uint8_t fnum[2];
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            long savedPos = ftell(f);
            fseek(f, (long)(recAbs + 4 + 20), SEEK_SET);
            bool gotBytes = (fread(fnum, 1, 2, f) == 2);
            fseek(f, savedPos, SEEK_SET);
            xSemaphoreGive(g_sd_mutex);
            if (!gotBytes) continue;

            uint32_t fl = ((uint32_t)fnum[0] << 8) | fnum[1];
            if (fl == hwm) {
                hwm_block_end = recAbs + rlen;
            } else if (fl > hwm) {
                // First flight past hwm found
                return (hwm_block_end > 0) ? hwm_block_end : recAbs;
            }
        }

        // Carry last 3 bytes across chunk boundary
        if (avail >= 3) {
            carry = 3;
            memmove(buf, buf + avail - 3, 3);
        } else {
            carry = avail;
            memmove(buf, buf, carry);
        }
        pos += got;
    }
    return hwm_block_end;  // 0 if we never found hwm record (unusual)
}

// ── OTA firmware update ─────────────────────────────────────────────────────

// versionNewer() — moved to airbridge_utils.h

static char g_otaTargetVer[16] = "";

// Update OTA fields on the display state (rendered by updateDisplay in main loop)
// Do NOT call doUpdateDisplay() here — this runs in the upload task and would
// race with main_loop_task's display rendering, causing screen corruption.
static void otaDisplayProgress(int pct, uint32_t received, uint32_t total) {
    g_displayState.otaActive = true;
    g_displayState.otaPct = pct;
    strlcpy(g_displayState.otaVersion, g_otaTargetVer, sizeof(g_displayState.otaVersion));
    (void)received; (void)total;
}

static bool otaDownloadAndFlash(const char* host, const char* path, uint32_t expectedSize) {
    // Download + flash with RESUME. A mid-stream link drop (the field failure that
    // left a unit on old firmware) no longer abandons the whole image: we keep the
    // esp_ota handle and reconnect with an HTTP Range header to continue from where
    // we stopped, up to OTA_MAX_ATTEMPTS times. esp_ota_write is sequential, so
    // continuing to write the next bytes into the same handle resumes cleanly.
    const int OTA_MAX_ATTEMPTS = 4;
    uint32_t total = expectedSize;   // full image size (from the version check)

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) { log_write("OTA: no partition"); return false; }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) { log_write("OTA: begin failed"); return false; }

    uint32_t received = 0;           // bytes flashed so far (persists across attempts)
    uint32_t otaT0 = millis();       // for download-bandwidth quality sample
    bool complete = false;
    bool hardFail = false;

    for (int attempt = 0; attempt < OTA_MAX_ATTEMPTS && !complete && !hardFail; attempt++) {
        g_tlsActive = true;
        if (attempt == 0)
            log_write("OTA: connecting to %s heap=%lu", host, (unsigned long)esp_get_free_heap_size());
        else
            log_write("OTA: resume attempt %d from %lu/%lu", attempt, (unsigned long)received, (unsigned long)total);

        esp_tls_t* tls = tls_connect(host);
        if (!tls) {
            log_write("OTA: TLS connect failed (attempt %d)", attempt);
            g_tlsActive = false;
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }

        // GET with a Range header when resuming (received>0).
        std::string req = buildRangeGetRequest(host, path, received);
        if (!tls_write_all(tls, req.c_str(), req.size())) {
            log_write("OTA: header send failed");
            tls_destroy(tls); g_tlsActive = false;
            continue;
        }

        // Read HTTP status line + headers.
        int status = 0, contentLength = 0;
        {
            char line[512];
            while (true) {
                int pos = 0;
                while (pos < (int)sizeof(line) - 1) {
                    char c;
                    int r = esp_tls_conn_read(tls, &c, 1);
                    if (r <= 0) goto hdr_done;
                    line[pos++] = c;
                    if (c == '\n') break;
                }
                line[pos] = '\0';
                if (status == 0 && strncmp(line, "HTTP/", 5) == 0) {
                    const char* sp = strchr(line, ' ');
                    if (sp) status = atoi(sp + 1);
                }
                if (strncasecmp(line, "Content-Length:", 15) == 0)
                    contentLength = atoi(line + 15);
                if (pos <= 2 && (line[0] == '\r' || line[0] == '\n')) break;
            }
        }
hdr_done:
        log_write("OTA: HTTP %d (have %lu/%lu)", status, (unsigned long)received, (unsigned long)total);

        // 200 = full body (expected on first attempt). 206 = partial (expected on
        // resume). If we asked to resume but got 200, the server ignored Range and
        // is resending from byte 0 — restart the handle cleanly to avoid corruption.
        if (received > 0 && status == 200) {
            log_write("OTA: server ignored Range — restarting from 0");
            esp_ota_abort(ota_handle);
            if (esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
                log_write("OTA: re-begin failed"); tls_destroy(tls); g_tlsActive = false; hardFail = true; break;
            }
            received = 0;
        } else if (!(status == 200 || status == 206)) {
            log_write("OTA: bad status %d", status);
            tls_destroy(tls); g_tlsActive = false;
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if (total == 0 && status == 200 && contentLength > 0) total = (uint32_t)contentLength;

        // Stream this connection's body into the OTA handle.
        char buf[2048];
        uint32_t lastProgressMs = millis();
        uint32_t lastProgressBytes = received;
        while (total == 0 || received < total) {
            int len = esp_tls_conn_read(tls, buf, sizeof(buf));
            if (len > 0) {
                err = esp_ota_write(ota_handle, buf, len);
                if (err != ESP_OK) {
                    log_write("OTA: flash write failed at %lu", (unsigned long)received);
                    tls_destroy(tls); g_tlsActive = false;
                    hardFail = true; break;
                }
                received += (uint32_t)len;
                if (total > 0 && (received % 16384) < (uint32_t)len)
                    otaDisplayProgress((received * 100) / total, received, total);
                if (received - lastProgressBytes >= 4096) {
                    lastProgressMs = millis();
                    lastProgressBytes = received;
                }
            } else if (len == 0) {
                break;                  // connection closed (maybe mid-stream)
            } else {
                log_write("OTA: read error at %lu/%lu", (unsigned long)received, (unsigned long)total);
                break;                  // link dropped — will resume on next attempt
            }
            if (millis() - lastProgressMs > 60000) {
                log_write("OTA: stalled at %lu/%lu", (unsigned long)received, (unsigned long)total);
                break;
            }
        }

        tls_destroy(tls);
        g_tlsActive = false;

        if (total > 0 && received >= total) complete = true;
        else if (attempt < OTA_MAX_ATTEMPTS - 1) vTaskDelay(pdMS_TO_TICKS(3000));  // backoff before resume
    }

    if (!complete) {
        log_write("OTA: incomplete %lu/%lu after retries", (unsigned long)received, (unsigned long)total);
        esp_ota_abort(ota_handle);
        return false;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) { log_write("OTA: end failed: %s", esp_err_to_name(err)); return false; }
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) { log_write("OTA: set_boot failed"); return false; }

    {   // OTA download is a real throughput sample — log it (quality bars use latency)
        float dt = (millis() - otaT0) / 1000.0f;
        if (dt > 0.5f) log_write("OTA: download %.0f KB/s", (received / 1024.0f) / dt);
    }
    log_write("OTA: success — %lu bytes", (unsigned long)received);
    otaDisplayProgress(100, received, total);
    return true;
}

// Returns: 1=updated (staged, needs reboot), 0=up to date, -1=transient error
// OTA check — uses shared halOtaCheck() for version check + URL,
// then ESP-IDF-specific otaDownloadAndFlash() for the actual flash.
static int otaCheck() {
    if (!g_netConnected && !g_pppConnected) return -1;
    g_tlsActive = true;

    log_write("OTA: checking for update (fw=%s)", FW_VERSION);

    // Steps 1-2: version check + download URL (shared code)
    OtaCheckResult ota = halOtaCheck(FW_VERSION);

    if (ota.status == 0) {
        log_write("OTA: up to date");
        return 0;
    }
    if (ota.status < 0) {
        log_write("OTA: check failed");
        return -1;
    }

    // Update available — switch display from "Checking..." to version + progress bar
    strlcpy(g_otaTargetVer, ota.newVersion, sizeof(g_otaTargetVer));
    g_displayState.otaActive = true;
    g_displayState.otaPct = 0;  // show version + empty progress bar
    strlcpy(g_displayState.otaVersion, ota.newVersion, sizeof(g_displayState.otaVersion));
    g_otaActive = true;
    log_write("OTA: update %s -> %s (%lu bytes)", FW_VERSION, ota.newVersion, (unsigned long)ota.size);

    // Step 3: Download and flash (ESP-IDF specific)
    char s3Host[128];
    static char s3Path[2500];
    if (!parseUrl(std::string(ota.downloadUrl), s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
        log_write("OTA: bad URL");
        g_otaActive = false;
        g_displayState.otaActive = false;
        return -1;
    }

    log_write("OTA: host=%s path_len=%d heap=%lu", s3Host, (int)strlen(s3Path), (unsigned long)esp_get_free_heap_size());

    if (!otaDownloadAndFlash(s3Host, s3Path, ota.size)) {
        log_write("OTA: download/flash failed heap=%lu", (unsigned long)esp_get_free_heap_size());
        g_otaActive = false;
        g_displayState.otaActive = false;
        return -1;
    }

    nvs_handle_t h;
    if (nvs_open("ota", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ota_status", "pending");
        nvs_commit(h);
        nvs_close(h);
    }

    log_write("OTA: v%s downloaded — ready to apply", ota.newVersion);
    g_otaActive = false;
    return 1;
}

// Upload a file from /sdcard/upload/<relPath> to S3.
// s3KeyOverride: if non-null, use as the full S3 key instead of {device}/{relPath}.
// startOffset: begin reading the file at this byte (0 = from start).
static bool s3UploadFileEx(const char* relPath, const char* s3KeyOverride,
                           uint64_t startOffset);

static bool s3UploadFile(const char* relPath) {
    return s3UploadFileEx(relPath, nullptr, 0);
}

static bool s3UploadFileEx(const char* relPath, const char* s3KeyOverride,
                           uint64_t startOffset) {
    if (!g_netConnected && !g_pppConnected) { cdc_printf("S3: no network\r\n"); return false; }
    if (!s3LoadCreds()) return false;
    g_tlsActive = true;  // suppress +++ for entire upload session

    char fpath[128];
    snprintf(fpath, sizeof(fpath), "%s/upload/%s", SD_MOUNT, relPath);

    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    FILE* f = fopen(fpath, "rb");
    uint32_t totalFileSize = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        totalFileSize = (uint32_t)ftell(f);
        // Seek to startOffset (0 for normal uploads, >0 for delta uploads)
        fseek(f, (long)startOffset, SEEK_SET);
    }
    xSemaphoreGive(g_sd_mutex);
    if (!f || totalFileSize == 0) {
        cdc_printf("S3: can't open %s", fpath);
        if (f) fclose(f);
        return false;
    }
    // Upload size = bytes from startOffset to EOF
    uint32_t fileSize = (startOffset < totalFileSize) ? (totalFileSize - (uint32_t)startOffset) : 0;
    if (fileSize == 0) {
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
        return true;  // nothing to upload (delta already covered)
    }

    static char s3Path[2500];  // shared, STS tokens are long

    // ── Small file: single pre-signed PUT ────────────────────────────────
    if (fileSize <= S3_CHUNK_SIZE) {
        std::string enc = urlEncode(relPath);
        char query[640];
        if (s3KeyOverride) {
            std::string encKey = urlEncode(s3KeyOverride);
            snprintf(query, sizeof(query), "file=%s&size=%u&device=%s&key=%s",
                     enc.c_str(), fileSize, g_deviceId, encKey.c_str());
        } else {
            snprintf(query, sizeof(query), "file=%s&size=%u&device=%s",
                     enc.c_str(), fileSize, g_deviceId);
        }
        std::string resp = s3ApiGet(query);

        // Skip upload if S3 already has this file (e.g. log uploaded via incremental)
        if (resp.find("\"skip\"") != std::string::npos &&
            resp.find("true") != std::string::npos) {
            log_write("S3: skip '%s' (already on S3)", relPath);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return true;  // treat as success — caller will delete the file
        }

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
        cdc_printf("S3: uploaded '%s' OK (%u bytes, %.0f KB/s)\r\n", relPath, fileSize, g_lastUploadKBps);
        log_write("Upload OK: %s %u bytes %.0f KB/s", relPath, fileSize, g_lastUploadKBps);
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
            if (strcmp(storedName, relPath) == 0) {
                uint32_t retries = 0;
                nvs_get_u32(h, "retries", &retries);
                if (retries >= 3) {
                    // Too many resume failures — start fresh
                    log_write("S3: stale session for %s (retries=%lu), clearing", relPath, (unsigned long)retries);
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
        std::string enc = urlEncode(relPath);
        char query[640];
        if (s3KeyOverride) {
            std::string encKey = urlEncode(s3KeyOverride);
            snprintf(query, sizeof(query), "file=%s&size=%u&device=%s&key=%s",
                     enc.c_str(), fileSize, g_deviceId, encKey.c_str());
        } else {
            snprintf(query, sizeof(query), "file=%s&size=%u&device=%s",
                     enc.c_str(), fileSize, g_deviceId);
        }
        std::string resp = s3ApiGet(query);

        // Skip if S3 already has this file
        if (resp.find("\"skip\"") != std::string::npos &&
            resp.find("true") != std::string::npos) {
            log_write("S3: skip '%s' (already on S3)", relPath);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return true;
        }

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
            nvs_set_str(h, "name", relPath);
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

    // Seek file to resume position. For delta uploads, startOffset is already applied
    // (file was opened and seeked to startOffset above); part offsets are relative to that.
    uint32_t resumeOffset = (startPart - 1) * S3_CHUNK_SIZE;
    uint64_t seekTarget = startOffset + resumeOffset;
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    fseek(f, (long)seekTarget, SEEK_SET);
    xSemaphoreGive(g_sd_mutex);

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
    log_write("Upload OK: %s %u bytes %u parts %.0f KB/s", relPath, fileSize, totalParts, g_lastUploadKBps);
    cdc_printf("S3: uploaded '%s' OK (%u bytes, %u parts, %.0f KB/s)\r\n",
             relPath, fileSize, totalParts, g_lastUploadKBps);
    return true;
}

// SKIP_NAMES[], isSkipped() — moved to airbridge_utils.h

// ── Cellular modem task (SIM7600 via raw UART + PPPoS) ──────────────────────

// UART helpers — route through HAL when available, else use raw ESP-IDF
int mdm_write(const void* data, size_t len) {
    if (g_hal && g_hal->uart) return g_hal->uart->write(data, len);
    return uart_write_bytes(UART_NUM_1, data, len);
}
int mdm_read(void* buf, size_t len, uint32_t timeout_ms) {
    if (g_hal && g_hal->uart) return g_hal->uart->read(buf, len, timeout_ms);
    return uart_read_bytes(UART_NUM_1, buf, len, pdMS_TO_TICKS(timeout_ms));
}
void mdm_flush() {
    if (g_hal && g_hal->uart) { g_hal->uart->flush(); return; }
    uart_flush(UART_NUM_1);
}
void mdm_set_baudrate(uint32_t baud) {
    if (g_hal && g_hal->uart) { g_hal->uart->set_baudrate(baud); return; }
    uart_set_baudrate(UART_NUM_1, baud);
}

// Send AT command, wait for response, return response string
int modem_at_cmd(const char* cmd, char* resp, int resp_size, int timeout_ms) {
    // Send command
    mdm_write(cmd, strlen(cmd));
    mdm_write("\r", 1);

    // Read response with timeout
    int total = 0;
    uint32_t start = millis();
    while ((millis() - start) < (uint32_t)timeout_ms && total < resp_size - 1) {
        uint8_t buf[128];
        int len = mdm_read(buf, sizeof(buf), 100);
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
    int written = mdm_write(buffer, len);
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
        // DNS backup (8.8.8.8) is configured in the modem task loop after the
        // interface is fully set up — not here, to avoid potential IPC deadlock
        // if esp_netif_set_dns_info dispatches to the lwIP core while the event
        // loop is already holding related locks.
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        cdc_printf("Modem: PPP lost IP\r\n");
        log_write("PPP lost IP");
        g_pppConnected = false;
        g_pppNeedsReconnect = true;
        // Don't reset RSSI — it's still valid from modem init or last reconnect
    }
}

// ── Modem health probe (diagnostic) ─────────────────────────────────────────
// Escapes PPP data mode, reads the REAL radio state (signal/registration/PDP
// context/IP), and returns to data mode. Used to understand the "zombie link"
// (PPP reports connected but no data flows). Disruptive (~10s pause of the data
// stream) — diagnostic only; gate off via -DMODEM_HEALTH_DEBUG=0.
#ifndef MODEM_HEALTH_DEBUG
#define MODEM_HEALTH_DEBUG 0   // probe disabled: modem confirmed healthy; avoid +++/ATO disrupting in-flight PUTs
#endif
#if MODEM_HEALTH_DEBUG
static void modemHealthProbe() {
    if (!g_pppConnected) return;
    char resp[256];
    // Escape PPP → command mode (+++ guard timing)
    vTaskDelay(pdMS_TO_TICKS(1100));
    mdm_write("+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));
    int drained = mdm_read((uint8_t*)resp, sizeof(resp) - 1, 600);
    if (drained > 0) { resp[drained] = '\0'; }
    bool inCmd = (drained > 0 && strstr(resp, "OK"));

    auto field = [&](const char* cmd, const char* tag, char* out, size_t osz) {
        out[0] = '\0';
        if (modem_at_cmd(cmd, resp, sizeof(resp), 3000) > 0) {
            char* p = strstr(resp, tag);
            if (p) {
                size_t k = 0;
                for (; p[k] && p[k] != '\r' && p[k] != '\n' && k < osz - 1; k++) out[k] = p[k];
                out[k] = '\0';
            }
        }
    };
    char csq[48], cereg[48], cgreg[48], cgact[48], cgpaddr[80];
    field("AT+CSQ",      "+CSQ:",     csq,     sizeof(csq));
    field("AT+CEREG?",   "+CEREG:",   cereg,   sizeof(cereg));
    field("AT+CGREG?",   "+CGREG:",   cgreg,   sizeof(cgreg));
    field("AT+CGACT?",   "+CGACT:",   cgact,   sizeof(cgact));
    field("AT+CGPADDR=1","+CGPADDR:", cgpaddr, sizeof(cgpaddr));
    log_write("MHEALTH escOK=%d | %s | %s | %s | %s | %s",
              (int)inCmd, csq, cereg, cgreg, cgact, cgpaddr);

    // Return to data mode
    int ato = modem_at_cmd("ATO", resp, sizeof(resp), 4000);
    bool back = (ato > 0 && strstr(resp, "CONNECT"));
    log_write("MHEALTH ATO=%s", back ? "CONNECT (data mode resumed)" : "FAILED (session dead)");
    if (!back) g_pppNeedsReconnect = true;  // dead session pppStale missed → force reconnect
}
#endif

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
    mdm_flush();

    char resp[512];

    // +++ escape (modem may be in PPP data mode from previous ESP32 boot)
    vTaskDelay(pdMS_TO_TICKS(1100));
    mdm_write("+++", 3);
    vTaskDelay(pdMS_TO_TICKS(1100));
    mdm_flush();

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
        const int tryBauds[] = { 921600, 460800, 2000000, 3000000 };
        for (int b = 0; b < 4 && !ready; b++) {
            // First try WITHOUT flow control (modem may not assert CTS in data mode)
            mdm_set_baudrate(tryBauds[b]);
            vTaskDelay(pdMS_TO_TICKS(100));
            mdm_flush();
            // +++ escape at this baud
            vTaskDelay(pdMS_TO_TICKS(1100));
            mdm_write("+++", 3);
            vTaskDelay(pdMS_TO_TICKS(1100));
            mdm_flush();
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
            mdm_flush();
            vTaskDelay(pdMS_TO_TICKS(1100));
            mdm_write("+++", 3);
            vTaskDelay(pdMS_TO_TICKS(1100));
            mdm_flush();
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
        mdm_set_baudrate(115200);
        mdm_flush();
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

    // ── Radio reset + echo-off + time sync — shared with emulator/tests ──
    // modemRunInitPre(): CFUN=0/1 (clear stale PPP/PDP), ATE0, CTZU, AT+CCLK
    // time sync. Baud is left unchanged so the upgrade below can run.
    ModemInitResult mr = modemRunInitPre();
    if (mr.epoch) {
        g_bootEpoch = mr.epoch;
        g_bootMs = millis();
        airbridge_log_set_time(g_bootEpoch, g_bootMs);
        log_write("Modem: time synced from CCLK (epoch=%lu)", (unsigned long)g_bootEpoch);
    }

    // ── Increase baud rate (try highest first, fall back) ─────────────
    {
        const int bauds[] = { 3000000, 2000000, 921600 };
        const int nBauds = sizeof(bauds) / sizeof(bauds[0]);
        bool upgraded = false;
        for (int i = 0; i < nBauds && !upgraded; i++) {
            cdc_printf("Modem: trying %d baud...\r\n", bauds[i]);
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "AT+IPR=%d", bauds[i]);
            modem_at_cmd(cmd, resp, sizeof(resp), 2000);
            // Modem responds OK at old baud, THEN switches

            vTaskDelay(pdMS_TO_TICKS(200));
            mdm_set_baudrate(bauds[i]);
            vTaskDelay(pdMS_TO_TICKS(200));
            mdm_flush();

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
                        cdc_printf("Modem: HW flow control enabled at %d\r\n", bauds[i]);
                        log_write("Modem: HW FC enabled at %d", bauds[i]);
                    } else {
                        // Flow control broke things — disable it
                        cdc_printf("Modem: HW flow control failed at %d, disabling\r\n", bauds[i]);
                        uart_set_hw_flow_ctrl(UART_NUM_1, UART_HW_FLOWCTRL_DISABLE, 0);
                        uart_set_pin(UART_NUM_1, PIN_MODEM_TX, PIN_MODEM_RX,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
                        modem_at_cmd("AT+IFC=0,0", resp, sizeof(resp), 2000);
                        log_write("Modem: HW FC failed at %d", bauds[i]);
                    }
                }
            } else {
                // Failed — modem is at new baud but ESP can't talk to it
                // Try to reset modem baud: send AT+IPR=115200 at the failed baud
                cdc_printf("Modem: %d failed, resetting to 115200...\r\n", bauds[i]);
                modem_at_cmd("AT+IPR=115200", resp, sizeof(resp), 2000);
                vTaskDelay(pdMS_TO_TICKS(200));
                mdm_set_baudrate(115200);
                vTaskDelay(pdMS_TO_TICKS(200));
                mdm_flush();
                // Verify recovery
                modem_at_cmd("AT", resp, sizeof(resp), 2000);
            }
        }
        if (!upgraded) {
            cdc_printf("Modem: staying at 115200\r\n");
            log_write("Modem: baud 115200 (upgrade failed)");
        }
    }

    // ── Antenna signal-survey mode (no PPP) ──────────────────────────────
    // Register, then loop sampling RSSI/RSRP/RSRQ/SINR forever. We NEVER dial
    // PPP, so these AT reads never collide with an upload (the reason mid-PPP
    // polling was removed). Swap antennas and watch the SURVEY lines live over
    // CDC serial. Triggered by the `survey` airbridge.cmd directive.
    if (g_surveyMode) {
        // Optional band lock so antennas are compared on identical RF (the modem's
        // own band/cell reselection swings RSRP 10+ dB and otherwise swamps any
        // antenna difference). CNMP/CNBP persist in modem NVS — `band=auto` restores.
        // Self-verifying: we log the modem's CNBP format + the per-sample band so a
        // wrong mask is obvious. Bands <=32 only (mask = 1<<(N-1)).
        if (g_surveyBand != 0) {
            char r[512];
            modem_at_cmd("AT+CNBP=?", r, sizeof(r), 3000);
            for (char* p = r; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
            log_write("SURVEY: CNBP format = %s", r);
            if (g_surveyBand < 0) {
                modem_at_cmd("AT+CNMP=2", r, sizeof(r), 5000);          // auto mode
                modem_at_cmd("AT+CNBP=0xFFFFFFFFFFFFFFFF,0xFFFFFFFFFFFFFFFF", r, sizeof(r), 5000);
                log_write("SURVEY: band lock CLEARED (auto)");
            } else {
                modem_at_cmd("AT+CNMP=38", r, sizeof(r), 5000);         // LTE only
                char cmd[80];
                uint32_t lo = (g_surveyBand <= 32) ? (1u << (g_surveyBand - 1)) : 0;
                snprintf(cmd, sizeof(cmd), "AT+CNBP=0xFFFFFFFFFFFFFFFF,0x00000000%08lX",
                         (unsigned long)lo);
                int rc = modem_at_cmd(cmd, r, sizeof(r), 5000);
                for (char* p = r; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
                log_write("SURVEY: lock LTE band %d via %s -> %s", g_surveyBand, cmd,
                          (rc > 0 ? r : "(no resp)"));
            }
            g_hal->clock->delay_ms(8000);   // let it re-register on the (un)locked band
        }

        ModemInitResult sr = {};
        modemRegisterAndReadSignal(sr);
        g_modemReady = true;
        log_write("SURVEY: registered=%d op=%s — sampling every %dms, no PPP",
                  sr.registered, sr.operatorName[0] ? sr.operatorName : "?", SURVEY_INTERVAL_MS);
        cdc_printf("SURVEY: registered=%d op=%s\r\n", sr.registered, sr.operatorName);
        uint32_t n = 0;
        for (;;) {
            ModemSignalSample s;
            modemSurveySample(&s);
            n++;
            g_modemRssi = s.rssi; g_modemRsrp = s.rsrp;
            g_modemRsrq = s.rsrq; g_modemSinr = s.sinr;
            strlcpy(g_modemBand, s.band, sizeof(g_modemBand));
            if (s.carrier[0]) strlcpy(g_modemOp, s.carrier, sizeof(g_modemOp));
            log_write("SURVEY %lu: carrier=%s band=%s RSSI=%d RSRP=%d RSRQ=%d SINR=%d",
                      (unsigned long)n, s.carrier[0] ? s.carrier : "?",
                      s.band[0] ? s.band : "?", s.rssi, s.rsrp, s.rsrq, s.sinr);
            vTaskDelay(pdMS_TO_TICKS(SURVEY_INTERVAL_MS));
        }
        // never returns
    }

    // ── Registration + RSSI/operator + APN + PPP dial — shared with emulator ──
    // modemRunInitPost(): AT+CEREG=1/AUTOCSQ, CEREG/CGREG registration wait,
    // CSQ, COPS, CGDCONT APN, ATD*99# dial (3 attempts). Runs after the baud
    // upgrade so the modem dials at the upgraded baud, same as before.
    modemRunInitPost(mr);
    g_modemRssi = mr.rssi;
    g_modemRsrp = mr.rsrp; g_modemRsrq = mr.rsrq; g_modemSinr = mr.sinr;
    strlcpy(g_modemBand, mr.band, sizeof(g_modemBand));
    if (mr.operatorName[0]) strlcpy(g_modemOp, mr.operatorName, sizeof(g_modemOp));
    if (!mr.registered)
        log_write("Modem: CEREG timeout — dialed unregistered");
    cdc_printf("Modem: %s RSSI=%d reg=%d\r\n", g_modemOp, g_modemRssi, mr.registered);
    log_write("Modem: operator=%s RSSI=%d RSRP=%d RSRQ=%d SINR=%d band=%s",
              g_modemOp, g_modemRssi, g_modemRsrp, g_modemRsrq, g_modemSinr,
              g_modemBand[0] ? g_modemBand : "?");
    bool connected = mr.connected;

    // Retry indefinitely if initial PPP dial fails.
    // NO CFUN=0/1 here — AT+CFUN=0 resets the SIM7600 UART to 115200 while the
    // host is still at the upgraded baud (3Mbaud), breaking all subsequent AT
    // commands silently. Soft reconnect (deactivate stale context, re-state APN,
    // redial) is sufficient to recover from a failed initial dial.
    while (!connected) {
        cdc_printf("Modem: PPP dial failed — retrying in 30s\r\n");
        log_write("Modem: PPP dial failed — retrying");
        vTaskDelay(pdMS_TO_TICKS(30000));
        // Wait up to 60s for registration before retrying (covers slow re-attach)
        for (int ci = 0; ci < 20; ci++) {
            modem_at_cmd("AT+CEREG?", resp, sizeof(resp), 1000);
            if (strstr(resp, ",1") || strstr(resp, ",5")) break;
            modem_at_cmd("AT+CGREG?", resp, sizeof(resp), 1000);
            if (strstr(resp, ",1") || strstr(resp, ",5")) break;
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        modem_at_cmd("AT+CGACT=0,1", resp, sizeof(resp), 5000);
        modem_at_cmd("AT+CGDCONT=1,\"IP\",\"hologram\"", resp, sizeof(resp), 5000);
        for (int dialAttempt = 0; dialAttempt < 3 && !connected; dialAttempt++) {
            cdc_printf("Modem: dialing PPP (attempt %d)...\r\n", dialAttempt + 1);
            mdm_write("ATD*99#\r", 8);
            uint32_t t0 = millis();
            char connbuf[256] = "";
            int connlen = 0;
            while (millis() - t0 < 30000) {
                int len = mdm_read((uint8_t*)connbuf + connlen,
                                          sizeof(connbuf) - 1 - connlen, 500);
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
    // Counts reconnect attempts that got CONNECT but no IP. Reset only when
    // g_pppConnected fires (IP obtained) — not on CONNECT alone — so the CFUN
    // backoff engages when stuck in the "no IP after CONNECT" loop all night.
    static int s_reconnect_failures = 0;
    static char urcBuf[128];
    static int urcLen = 0;

    while (true) {
        uint8_t buf[1024];
        int len = mdm_read(buf, sizeof(buf), 20);
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
            s_reconnect_failures = 0;  // IP obtained — full session up, reset counter

            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(g_ppp_netif, &ip_info) == ESP_OK) {
                cdc_printf("Modem: PPP ip=" IPSTR " gw=" IPSTR "\r\n",
                    IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));
            }

            struct netif* ppp_lwip_netif = (struct netif*)esp_netif_get_netif_impl(g_ppp_netif);
            if (ppp_lwip_netif) netif_set_default(ppp_lwip_netif);
            esp_netif_set_default_netif(g_ppp_netif);
            cdc_printf("Modem: cellular ready — set as default route\r\n");

            // Log carrier DNS and inject 8.8.8.8 as backup.
            // Done here (modem task) rather than in the IP event handler to avoid
            // potential IPC deadlock in the event loop context.
            {
                esp_netif_dns_info_t d = {};
                if (esp_netif_get_dns_info(g_ppp_netif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK)
                    log_write("PPP DNS main: " IPSTR, IP2STR(&d.ip.u_addr.ip4));
                d.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
                d.ip.type = ESP_IPADDR_TYPE_V4;
                if (esp_netif_set_dns_info(g_ppp_netif, ESP_NETIF_DNS_BACKUP, &d) == ESP_OK)
                    log_write("PPP DNS backup: 8.8.8.8");
            }
            // Log filename already set at boot (boot_NNNN)
        }

        // ── Track last PPP data for stale detection ─────────────────────
        static uint32_t lastPppRxMs = 0;
        if (len > 0) lastPppRxMs = millis();

#if MODEM_HEALTH_DEBUG
        // ── Periodic modem-health probe (diagnostic) ────────────────────
        // Every 60s while connected, read the real radio/PDP state so a CDC
        // capture shows whether the link goes zombie (PPP up, radio dropped).
        static uint32_t lastProbeMs = 0;
        if (g_pppConnected && (millis() - lastProbeMs) > 60000) {
            lastProbeMs = millis();
            modemHealthProbe();
            lastPppRxMs = millis();  // probe paused PPP — don't let it trip pppStale
        }
#endif

        // Periodic +++ RSSI refresh removed — it disrupts PPP/TLS.
        // RSSI is read once during modem init.

        // ── Detect PPP connection loss or stuck CONNECT without IP ────────
        // 90s baseline avoids false positives from the 30s LCP echo interval:
        // if one echo-reply is delayed or reordered, a 30s window would fire on
        // a perfectly healthy link. TLS slow-handshakes need even more headroom.
        uint32_t staleMs = g_tlsActive ? 120000 : 90000;
        bool pppStale = g_pppConnected && lastPppRxMs > 0 &&
                        (millis() - lastPppRxMs) > staleMs;
        // If we got CONNECT but no IP within 30s, force reconnect.
        // Increment failure counter here so the CFUN backoff engages on
        // repeated "no IP" cycles — rr.connected=true doesn't increment it.
        if (!g_pppConnected && !g_pppNeedsReconnect && lastPppRxMs > 0 &&
            (millis() - lastPppRxMs) > 30000) {
            s_reconnect_failures++;
            log_write("Modem: no IP after CONNECT — forcing reconnect (failure #%d)",
                      s_reconnect_failures);
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
            // Keep g_modemRssi — display shows last-known signal during reconnect

            // Soft reconnect (no CFUN) for the first 4 attempts — soft now includes
            // a +++ guard so it handles the "modem stuck in PPP data mode" case.
            // CFUN=0/1 (full radio reset) starts at attempt 5 and every 5th after.
            // Escalation/backoff is the shared, unit-tested modemReconnectPlan().
            ModemReconnectPlan plan = modemReconnectPlan(s_reconnect_failures);
            bool doRadioReset = plan.radioReset;
            if (plan.backoffSeconds > 0) {
                log_write("Modem: backoff %lus before radio reset (failure #%d)",
                          (unsigned long)plan.backoffSeconds, s_reconnect_failures);
                cdc_printf("Modem: backoff %lus before radio reset\r\n", (unsigned long)plan.backoffSeconds);
                vTaskDelay(pdMS_TO_TICKS(plan.backoffSeconds * 1000));
            }
            log_write("Modem: reconnect attempt %d (%s)",
                      s_reconnect_failures + 1, doRadioReset ? "full reset" : "soft");
            cdc_printf("Modem: reconnecting (attempt %d, %s)...\r\n",
                       s_reconnect_failures + 1, doRadioReset ? "full reset" : "soft");
            ModemReconnectResult rr = modemReconnect(doRadioReset);

            if (rr.registered) {
                g_modemRssi = rr.rssi;
                strlcpy(g_modemOp, rr.operatorName, sizeof(g_modemOp));
                log_write("Reconnect: rssi=%d op=%s", rr.rssi, rr.operatorName);
                cdc_printf("Modem: reconnect rssi=%d op=%s\r\n", rr.rssi, rr.operatorName);
            }

            if (rr.connected) {
                // Do NOT reset s_reconnect_failures here — we have CONNECT but
                // no IP yet.  Reset happens below when g_pppConnected fires.
                lastPppRxMs = millis();
                test_launched = false;
                cdc_printf("Modem: PPP CONNECT — negotiating...\r\n");
                log_write("Modem: PPP redial CONNECT (attempt %d)", s_reconnect_failures + 1);
                // Full PPP state machine reset: stop→start→connected.
                // After many reconnect cycles esp_netif_action_connected() alone
                // fails to restart LCP — the state machine accumulates STOPPED/CLOSED
                // state across cycles and ignores the connected transition.
                esp_netif_action_disconnected(g_ppp_netif, nullptr, 0, nullptr);
                esp_netif_action_stop(g_ppp_netif, nullptr, 0, nullptr);
                esp_netif_action_start(g_ppp_netif, nullptr, 0, nullptr);
                esp_netif_action_connected(g_ppp_netif, nullptr, 0, nullptr);
            } else if (!rr.registered) {
                s_reconnect_failures++;
                // Signal-gated retry: poll coverage every 10s and redial the MOMENT the
                // network returns (instead of a blind 60s wait). Returns early when
                // registered; caps at 60s so a CFUN-reset escalation can still kick in.
                cdc_printf("Modem: not registered — polling coverage every 10s\r\n");
                log_write("Modem: not registered — signal-gated retry");
                ModemSignalWait sw = modemWaitForSignal(60000, 10000);
                if (sw.registered && sw.rssi != 99) g_modemRssi = sw.rssi;
                g_pppNeedsReconnect = true;
            } else {
                s_reconnect_failures++;
                log_write("Modem: redial failed — signal-gated retry");
                cdc_printf("Modem: redial failed — polling coverage every 10s\r\n");
                // Registered but redial failed → modemWaitForSignal returns ~immediately
                // (already registered) → fast retry, no wasted minute.
                ModemSignalWait sw = modemWaitForSignal(60000, 10000);
                if (sw.registered && sw.rssi != 99) g_modemRssi = sw.rssi;
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

        // ── Remote command channel — HIGHEST-priority C2 (survives a wedged unit) ──
        // Fetch + run remote commands FIRST after PPP is up (before OTA/cookie/harvest),
        // then every 5 min. Runs on the modem task (independent of the SD/harvest/upload
        // paths that wedge) and executes IN-MEMORY with allowSdOps=false (never blocks on
        // the SD mutex) — so a unit whose SD is the problem is still recoverable over
        // cellular: format_sd → NVS sys/format flag + esp_restart (no SD/harvest needed).
        // The acceptance ack is POSTed BEFORE any restart so a reboot can't swallow it.
        {
            static uint32_t lastCmdPollMs = 0;
            static bool     cmdPolledThisConnect = false;
            if (!g_pppConnected) cmdPolledThisConnect = false;
            bool firstCmd = (g_pppConnected && !cmdPolledThisConnect && sinceConnect > 5000);
            if (g_pppConnected && !g_surveyMode && g_deviceId[0] &&
                (firstCmd || (millis() - lastCmdPollMs) > 300000)) {
                lastCmdPollMs = millis();
                cmdPolledThisConnect = true;
                static char cmdText[1024];
                if (halFetchCommands(g_deviceId, cmdText, sizeof(cmdText)) && cmdText[0]) {
                    airbridge_log("CMD: remote command received (%u bytes)", (unsigned)strlen(cmdText));
                    CmdRunResult cr = runCommandTextBuffer(
                        cmdText, /*runtimeOnly=*/false, "/sdcard/diag", "/sdcard/logs",
                        "/sdcard/upload", nullptr, 0, /*allowSdOps=*/false);
                    // Acceptance confirmation — POST BEFORE any restart.
                    char ack[320];
                    snprintf(ack, sizeof(ack),
                        "{\"fw\":\"%s\",\"ran\":%s,\"reboot\":%s,\"format\":%s,\"cdc\":%s,"
                        "\"survey\":%s,\"compress\":%s,\"uptime_s\":%lu}",
                        FW_VERSION, cr.ran?"true":"false", cr.reboot?"true":"false",
                        cr.format?"true":"false", cr.cdc?"true":"false", cr.survey?"true":"false",
                        g_compress?"true":"false", (unsigned long)(millis()/1000));
                    halAckCommand(g_deviceId, ack);
                    if (cr.cdc)     g_msc_only = false;                       // effective next boot
                    if (cr.survey){ g_surveyMode = true; g_surveyBand = cr.surveyBand; }
                    if (cr.format) {
                        airbridge_log("CMD: format_sd — reformat on next boot (remote)");
                        nvs_handle_t fh;
                        if (nvs_open("sys", NVS_READWRITE, &fh) == ESP_OK) {
                            nvs_set_u8(fh, "format", 1); nvs_commit(fh); nvs_close(fh);
                        }
                        if (!g_harvesting) sd_before_restart();  // skip teardown if SD busy (race)
                        esp_restart();
                    } else if (cr.reboot) {
                        airbridge_log("CMD: reboot (remote)");
                        if (!g_harvesting) sd_before_restart();
                        esp_restart();
                    }
                }
            }
        }

        // Flush RAM log to SD file (every 30s, needs SD mutex)
        // Use 500ms timeout — 50ms was too tight when USB MSC held the SPI bus,
        // causing silent flush failures and ring-buffer wrap after ~10 minutes.
        {
            static uint32_t lastFlushMs = 0;
            if (s_loglen > 0 && g_fatfs_mounted && !g_harvesting &&
                (millis() - lastFlushMs) > 30000) {
                if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                    log_flush_to_sd();
                    xSemaphoreGive(g_sd_mutex);
                    lastFlushMs = millis();
                }
            }
        }

        // ── SD-independent log egress (degraded mode) ────────────────────
        // When the SD card is unavailable (corrupt/unmounted) the normal
        // SD-file upload below is a no-op (no file to read), so the device
        // would go dark with no way to report *why*. Instead, POST the RAM
        // ring buffer straight to S3 so an operator can still see the unit is
        // alive and its SD failed. The modem/PPP path needs no SD, and the
        // session id (g_logFileName) comes from NVS, not the card — so this
        // works even with a totally dead FAT. On success we consume exactly
        // the bytes sent (airbridge_log_consume) so each line egresses once.
        if (g_pppConnected && !logUpRunning && !g_fatfs_mounted && g_logFileName[0] &&
            (firstUpload || (logInterval > 0 && (millis() - lastLogUploadMs) > logInterval))) {
            lastLogUploadMs = millis();
            logUpRunning = true;
            xTaskCreatePinnedToCore([](void*) {
                do {
                    if (!s3LoadCreds()) break;
                    static char snap[AIRBRIDGE_LOG_BUF_SIZE];
                    int snapLen = airbridge_log_snapshot(snap, sizeof(snap));
                    if (snapLen <= 0) break;

                    esp_tls_t* tls = tls_connect(g_apiHost);
                    if (!tls) break;
                    char hdr[512];
                    int hlen = snprintf(hdr, sizeof(hdr),
                        "POST /prod/log/append?device=%s&session=%s HTTP/1.1\r\n"
                        "Host: %s\r\nx-api-key: %s\r\n"
                        "Content-Length: %d\r\nConnection: close\r\n\r\n",
                        g_deviceId, g_logFileName, g_apiHost, g_apiKey, snapLen);
                    uint32_t wT0 = millis();
                    bool ok = tls_write_all(tls, hdr, hlen) &&
                              tls_write_all(tls, snap, snapLen);
                    if (ok) {
                        httpReadResponse(tls);
                        airbridge_log_consume(snapLen);  // drop only what we sent
                        noteLinkLatency(millis() - wT0);
                    }
                    tls_destroy(tls);
                } while (0);
                logUpRunning = false;
                vTaskDelete(nullptr);
            }, "log_up_ram", 8192, nullptr, 2, nullptr, 1);
        }

        // Upload log to S3 via /prod/log/append (incremental, per-session)
        static long lastUploadPos = 0;
        if (g_pppConnected && !logUpRunning && g_fatfs_mounted && g_logFileName[0] &&
            (firstUpload || (logInterval > 0 && (millis() - lastLogUploadMs) > logInterval))) {

            // Check SD log file size
            char logPath[64];
            snprintf(logPath, sizeof(logPath), "/sdcard/logs/%s.log", g_logFileName);
            long fileSize = 0;
            bool mutexGot = xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(500)) == pdTRUE;
            if (mutexGot) {
                FILE* f = fopen(logPath, "r");
                if (f) { fseek(f, 0, SEEK_END); fileSize = ftell(f); fclose(f); }
                xSemaphoreGive(g_sd_mutex);
            }
            // Diagnostic: log why upload isn't firing (fires once per connect)
            static bool logAppendDiagDone = false;
            if (!logAppendDiagDone && firstUpload) {
                logAppendDiagDone = true;
                log_write("logAppend: file=%s size=%ld pos=%ld mutex=%d fatfs=%d",
                          logPath, fileSize, lastUploadPos,
                          (int)mutexGot, (int)g_fatfs_mounted);
            }

            if (fileSize > lastUploadPos) {
                lastLogUploadMs = millis();
                logUpRunning = true;
                long uploadFrom = lastUploadPos;
                int uploadLen = (int)(fileSize - uploadFrom);
                if (uploadLen > 8192) uploadLen = 8192;  // max 8KB per append

                xTaskCreatePinnedToCore([](void* param) {
                    long* p = (long*)param;
                    long offset = p[0]; int len = (int)p[1];
                    delete[] p;
                    do {
                        if (!s3LoadCreds()) break;

                        // Read new log chunk from SD
                        static char chunk[8192];
                        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                        char lp[64];
                        snprintf(lp, sizeof(lp), "/sdcard/logs/%s.log", g_logFileName);
                        FILE* f = fopen(lp, "r");
                        int readLen = 0;
                        if (f) {
                            fseek(f, offset, SEEK_SET);
                            readLen = fread(chunk, 1, len, f);
                            fclose(f);
                        }
                        xSemaphoreGive(g_sd_mutex);
                        if (readLen == 0) break;

                        // POST to /prod/log/append
                        esp_tls_t* tls = tls_connect(g_apiHost);
                        if (!tls) break;

                        char hdr[512];
                        int hlen = snprintf(hdr, sizeof(hdr),
                            "POST /prod/log/append?device=%s&session=%s HTTP/1.1\r\n"
                            "Host: %s\r\nx-api-key: %s\r\n"
                            "Content-Length: %d\r\nConnection: close\r\n\r\n",
                            g_deviceId, g_logFileName, g_apiHost, g_apiKey, readLen);

                        uint32_t wT0 = millis();
                        bool ok = tls_write_all(tls, hdr, hlen) &&
                                  tls_write_all(tls, chunk, readLen);
                        if (ok) {
                            httpReadResponse(tls);
                            lastUploadPos = offset + readLen;
                            // Round-trip latency of this small POST (post-handshake)
                            // is the always-on connection-quality sample driving the
                            // OLED bars — too small to measure bandwidth, but latency
                            // tracks link quality and costs no extra traffic.
                            noteLinkLatency(millis() - wT0);
                        }
                        tls_destroy(tls);
                    } while (0);
                    logUpRunning = false;
                    vTaskDelete(nullptr);
                }, "log_up", 8192, new long[2]{uploadFrom, uploadLen}, 2, nullptr, 1);
            }
        }
    }
}

// Progress callback for the shared upload code (airbridge_s3.h) — drives the
// OLED upload-progress field. sent/total are per-PUT (single or per-part).
static void uploadProgressCb(uint32_t sent, uint32_t total) {
    (void)total;
    g_uploadingMb = g_uploadBaseMb + sent / 1e6f;
}

// ── Upload task ─────────────────────────────────────────────────────────────
static void uploadTask(void* param) {
    (void)param;
    static bool otaDone = false;

    // Deferred format now runs at boot (before task creation) — see app_main.

    // ── SD flash: firmware.bin on SD (runs before network, instant) ────
    if (g_fatfs_mounted) {
        char fwp[64];
        snprintf(fwp, sizeof(fwp), "%s/firmware.bin", SD_MOUNT);
        struct ::stat fwst;
        if (::stat(fwp, &fwst) == 0 && fwst.st_size > 100000) {
            log_write("SD flash: firmware.bin found (%ld bytes)", (long)fwst.st_size);
            cdc_printf("SD flash: firmware.bin found (%ld bytes)\r\n", (long)fwst.st_size);

            char tmp[64];
            snprintf(tmp, sizeof(tmp), "%s/_firmware.bin", SD_MOUNT);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            rename(fwp, tmp);
            xSemaphoreGive(g_sd_mutex);

            const esp_partition_t* update = esp_ota_get_next_update_partition(
                esp_ota_get_running_partition());
            esp_ota_handle_t oh;
            if (update && esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &oh) == ESP_OK) {
                xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                FILE* f = fopen(tmp, "rb");
                xSemaphoreGive(g_sd_mutex);
                char buf[4096];
                uint32_t rx = 0;
                bool ok = true;
                while (f && ok) {
                    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                    size_t n = fread(buf, 1, sizeof(buf), f);
                    xSemaphoreGive(g_sd_mutex);
                    if (n == 0) break;
                    if (esp_ota_write(oh, buf, n) != ESP_OK) ok = false;
                    rx += n;
                    if ((rx % 65536) < n)
                        cdc_printf("SD flash: %lu/%ld bytes\r\n", (unsigned long)rx, (long)fwst.st_size);
                }
                if (f) { xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex); }

                if (ok && rx == (uint32_t)fwst.st_size &&
                    esp_ota_end(oh) == ESP_OK &&
                    esp_ota_set_boot_partition(update) == ESP_OK) {
                    xSemaphoreTake(g_sd_mutex, portMAX_DELAY); remove(tmp); xSemaphoreGive(g_sd_mutex);
                    log_write("SD flash: success — rebooting");
                    cdc_printf("SD flash: OK — rebooting\r\n");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    sd_before_restart();
                    esp_restart();
                } else {
                    esp_ota_abort(oh);
                    cdc_printf("SD flash: FAILED\r\n");
                }
            }
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); remove(tmp); xSemaphoreGive(g_sd_mutex);
        }
    }

    // Show "Checking for update..." on display while waiting for network
    g_displayState.otaActive = true;
    g_displayState.otaPct = -1;

    // Wait for network — use the USB delay time productively.
    // If network comes up, do OTA + cookie before presenting USB to host.
    // Timeout: 120s from boot (gives cellular time, but doesn't block forever)
    {
        uint32_t bootMs = millis();
        uint32_t deadline = 120000;
        while (!g_netConnected && !g_pppConnected) {
            if (millis() - bootMs > deadline) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Brief pause for network stack to stabilize (default route, DNS)
    if (g_pppConnected) vTaskDelay(pdMS_TO_TICKS(2000));

    // ── OTA check: retry for the full 90s pre-USB window ────────────────────
    // Keeps retrying on transient failures (TLS connect, server busy) until
    // either the update succeeds, the version is current, or the 90s USB
    // presentation deadline arrives. Prevents premature "main screen" display.
    if (!otaDone && (g_netConnected || g_pppConnected)) {
        int otaResult = -1;
        uint32_t otaWindowEnd = g_bootMs + 88000;  // stop 2s before 90s USB present
        int attempt = 0;
        while (millis() < otaWindowEnd) {
            if (attempt > 0) {
                // 5s pause between retries; abort if window closing or network lost
                for (int w = 0; w < 50 && millis() < otaWindowEnd; w++)
                    vTaskDelay(pdMS_TO_TICKS(100));
                if (!g_netConnected && !g_pppConnected) {
                    // Network dropped — wait for it to recover before retrying
                    vTaskDelay(pdMS_TO_TICKS(5000));
                    if (!g_netConnected && !g_pppConnected) break;
                }
            }
            log_write("OTA: check attempt %d (t=%lus)", attempt + 1,
                      (unsigned long)((millis() - g_bootMs) / 1000));
            otaResult = otaCheck();  // 1=downloaded, 0=up to date, -1=error
            g_tlsActive = false;
            if (otaResult >= 0) break;  // success or confirmed up-to-date → done
            attempt++;
        }
        otaDone = true;
        g_displayState.otaActive = false;  // clear "Checking..." after window closes
        g_otaActive = false;
        bool staged = (otaResult == 1);

        if (staged) {
            // OTA downloaded — reboot immediately unless host is actively writing
            uint32_t lastWr = g_lastWriteMs;
            if (lastWr != 0 && (millis() - lastWr) < QUIET_WINDOW_MS) {
                log_write("OTA: waiting for host writes to settle");
                while (millis() - g_lastWriteMs < QUIET_WINDOW_MS) {
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
            log_write("OTA: rebooting to apply update");
            cdc_printf("OTA: rebooting...\r\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            sd_before_restart();
            esp_restart();
        }
    }
    // Clear checking display whether OTA ran or not (network timeout)
    g_displayState.otaActive = false;
    g_otaActive = false;

    // SD flash runs at top of uploadTask — no duplicate needed here

    // ── Check S3 for custom DSU cookie override ─────────────────────
    if ((g_netConnected || g_pppConnected) && s3LoadCreds()) {
        g_tlsActive = true;
        esp_tls_t* tls = tls_connect(g_apiHost);
        if (tls) {
            char req[512];
            int rlen = snprintf(req, sizeof(req),
                "GET /prod/firmware/cookie?device=%s HTTP/1.1\r\n"
                "Host: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
                g_deviceId, g_apiHost, g_apiKey);
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
                        if (write_cookie_to_dsu(cookie, 78)) {
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

    // ── Per-aircraft S3 cookie override ─────────────────────────────────────
    // Admin can push a date-mode or flight-mode cookie per DSU serial.
    // Stored at aircraft/{serial}/cookie.easdf — one-shot, deleted after fetch.
    // Takes priority over manifest HWM sync (runs before it).
    if (!g_s3CookieActive && (g_netConnected || g_pppConnected) && s3LoadCreds()) {
        // Get DSU serial from local cookie or NVS cache
        char acSerial[44] = "";
        if (!g_dual_partition) {
            char cp[64];
            snprintf(cp, sizeof(cp), "%s/dsuCookie.easdf", SD_MOUNT);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            FILE* cf = fopen(cp, "rb");
            if (cf) {
                uint8_t ck[78];
                if (fread(ck, 1, 78, cf) == 78 && ck[0] == 0xEA && ck[1] == 0x1E) {
                    size_t n = sizeof(acSerial) - 1; if (n > 42) n = 42;
                    memcpy(acSerial, ck + 9, n); acSerial[n] = '\0';
                    for (int j = (int)n - 1; j >= 0; j--) {
                        if ((uint8_t)acSerial[j] < 0x20 || (uint8_t)acSerial[j] > 0x7E)
                            acSerial[j] = '\0'; else break;
                    }
                }
                fclose(cf);
            }
            xSemaphoreGive(g_sd_mutex);
        }
        if (!acSerial[0]) {
            nvs_handle_t nh;
            if (nvs_open("mfst", NVS_READONLY, &nh) == ESP_OK) {
                nvs_get_string(nh, "serial", acSerial, sizeof(acSerial)); nvs_close(nh);
            }
        }
        if (acSerial[0]) {
            g_tlsActive = true;
            esp_tls_t* tls = tls_connect(g_apiHost);
            if (tls) {
                char req[512];
                int rlen = snprintf(req, sizeof(req),
                    "GET /prod/aircraft/cookie?serial=%s HTTP/1.1\r\n"
                    "Host: %s\r\nx-api-key: %s\r\nConnection: close\r\n\r\n",
                    acSerial, g_apiHost, g_apiKey);
                if (tls_write_all(tls, req, rlen)) {
                    std::string resp = httpReadResponse(tls);
                    std::string hexStr = jsonStr(resp, "cookie");
                    if (hexStr.size() == 156) {
                        uint8_t cookie[78];
                        for (int i = 0; i < 78; i++) {
                            char h[3] = { hexStr[i*2], hexStr[i*2+1], 0 };
                            cookie[i] = (uint8_t)strtoul(h, nullptr, 16);
                        }
                        if (cookie[0] == 0xEA && cookie[1] == 0x1E) {
                            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                            if (write_cookie_to_dsu(cookie, 78)) {
                                g_s3CookieActive = true;
                                log_write("Aircraft cookie applied: %s", acSerial);
                                cdc_printf("Aircraft cookie: applied for %s\r\n", acSerial);
                            }
                            xSemaphoreGive(g_sd_mutex);
                        }
                    } else if (resp.find("404") == std::string::npos) {
                        log_write("Aircraft cookie: none for %s", acSerial);
                    }
                }
                tls_destroy(tls);
            }
            g_tlsActive = false;
        }
    }

    // Old boot logs are moved to SD root at boot (see app_main) so the
    // normal harvest → upload pipeline handles them. No TLS needed here.

    // ── Manifest-based cookie sync (fleet-aware) ──────────────────────────
    // On AirBridge swap: the local cookie may be stale. Query S3 manifest for
    // the DSU serial (from NVS cache or current dsuCookie on SD) and advance
    // the local cookie to the S3 high_water_mark if it's higher.
    if (!g_s3CookieActive && (g_netConnected || g_pppConnected) && s3LoadCreds()) {
        // Read DSU serial + local flight from the cookie on SD (single-partition only;
        // dual-partition P1 is not mounted at this point — fall back to NVS cache).
        char bootSerial[44] = "";
        uint32_t bootLocalFlight = 0;
        {
            // Read the DSU cookie from the partition the DSU actually uses: P1 (/dsu)
            // in dual-partition mode (temp-mounted here), else P2 (/sdcard). This block
            // used to be single-partition-only, so dual-partition devices never read the
            // local cookie and the boot sync relied solely on the NVS cache (TEST 19-C).
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            bool dsuTmp = false;
            const char* ckDir = SD_MOUNT;
            if (g_dual_partition) { dsuTmp = mount_dsu(); ckDir = DSU_MOUNT; }
            char cookiePath[64];
            snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", ckDir);
            FILE* cf = (!g_dual_partition || dsuTmp) ? fopen(cookiePath, "rb") : nullptr;
            if (cf) {
                uint8_t ck[78];
                if (fread(ck, 1, 78, cf) == 78 && ck[0] == 0xEA && ck[1] == 0x1E) {
                    size_t copyLen = sizeof(bootSerial) - 1;
                    if (copyLen > 42) copyLen = 42;
                    memcpy(bootSerial, ck + 9, copyLen);
                    bootSerial[copyLen] = '\0';
                    for (int j = (int)copyLen - 1; j >= 0; j--) {
                        if ((uint8_t)bootSerial[j] < 0x20 || (uint8_t)bootSerial[j] > 0x7E)
                            bootSerial[j] = '\0';
                        else break;
                    }
                    bootLocalFlight = ((uint32_t)ck[62] << 24) | ((uint32_t)ck[63] << 16)
                                    | ((uint32_t)ck[64] << 8) | ck[65];
                    if (bootLocalFlight == 0xFFFFFFFF) { bootLocalFlight = 0; bootSerial[0] = '\0'; }
                }
                fclose(cf);
            }
            if (dsuTmp) unmount_dsu();
            xSemaphoreGive(g_sd_mutex);
        }
        // Fall back to NVS cache for serial when SD cookie unavailable
        if (!bootSerial[0]) {
            nvs_handle_t nh;
            if (nvs_open("mfst", NVS_READONLY, &nh) == ESP_OK) {
                nvs_get_string(nh, "serial", bootSerial, sizeof(bootSerial));
                uint32_t nhwm = 0;
                nvs_get_u32(nh, "hwm", &nhwm);
                if (nhwm > bootLocalFlight) bootLocalFlight = nhwm;
                nvs_close(nh);
            }
        }
        if (bootSerial[0]) {
            uint32_t s3Hwm = s3FetchManifest(bootSerial);
            log_write("Boot manifest sync: %s local=%lu S3=%lu",
                      bootSerial, (unsigned long)bootLocalFlight, (unsigned long)s3Hwm);
            // Write cookie from S3 HWM when it's higher than local (normal forward sync).
            // For admin-driven backward rewinding, use the PUT /aircraft/manifest endpoint
            // combined with force_cookie (TODO: implement force_cookie flag).
            if (s3Hwm > bootLocalFlight) {
                uint8_t syncCookie[78];
                buildDsuCookie(bootSerial, s3Hwm, syncCookie);
                xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                write_cookie_to_dsu(syncCookie, 78);
                xSemaphoreGive(g_sd_mutex);
                log_write("Boot cookie synced to S3 hwm=%lu", (unsigned long)s3Hwm);
                cdc_printf("Boot cookie sync: %s hwm=%lu\r\n", bootSerial, (unsigned long)s3Hwm);
            }
        }
        g_tlsActive = false;
    }

    // Signal main loop: OTA + cookie done, safe to present USB to host
    g_preUsbDone = true;
    log_write("Pre-USB tasks done — USB can be presented");
    cdc_printf("Pre-USB: OTA+cookie done\r\n");

    // ── Upload loop — scan /upload/NNNN/ subfolders ──────────────
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));  // wake on notify or every 15s

        int uploadedThisPass = 0;  // for the "queue drained" marker (E2E wait anchor)
        for (;;) {
            if (g_harvesting) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }

            char harvBase[64];
            snprintf(harvBase, sizeof(harvBase), "%s/upload", SD_MOUNT);

            // Find the next file to upload (oldest folder, lowest flight first).
            // Shared scanner — guard with the SD mutex since it walks dirs the
            // USB MSC task may also touch.
            char relPath[128] = "";  // "NNNN/filename"
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            bool found = findNextUploadFile(harvBase, relPath, sizeof(relPath));
            xSemaphoreGive(g_sd_mutex);
            if (!found) {
                // Queue drained. Log once per pass that did work so E2E waits have
                // an exact "upload idle" anchor on the CDC serial stream.
                if (uploadedThisPass > 0) {
                    log_write("Upload: queue drained (%d file(s) this pass)", uploadedThisPass);
                }
                break;  // nothing to upload
            }

            char path[192];
            snprintf(path, sizeof(path), "%s/%s", harvBase, relPath);
            cdc_printf("Upload: found %s\r\n", path);

            // File size for the display + queue accounting
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            float fileMb = 0.0f;
            { struct stat st; if (stat(path, &st) == 0) fileMb = (float)st.st_size / 1e6f; }
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

            // Is this a DSU flight log? (strip harvest "__" prefix for the bare name)
            const char* uploadFname = strrchr(relPath, '/');
            uploadFname = uploadFname ? uploadFname + 1 : relPath;
            const char* bareFname = uploadFname;
            for (const char* p = uploadFname; p[0] && p[1]; p++)
                if (p[0] == '_' && p[1] == '_') bareFname = p + 2;
            char eaSerial[44] = ""; uint32_t eaLast = 0;
            bool isEaofh = parseEaofhFilename(bareFname, eaSerial, sizeof(eaSerial), &eaLast);

            cdc_printf("Uploading: %s (%.1f MB) heap=%lu min=%lu\r\n", relPath, fileMb,
                       (unsigned long)esp_get_free_heap_size(),
                       (unsigned long)esp_get_minimum_free_heap_size());
            g_uploadingMb = 0.0f; g_uploadBaseMb = 0.0f;

            // Upload via the shared code path — identical to what the emulator runs.
            // .eaofh files go through fleet-aware manifest/delta logic; others are
            // a plain single/multipart PUT. SD reads inside hold g_sd_mutex via the
            // Esp32Filesys lock() hook; TLS uses the same tls_connect() as before.
            UploadResult ur = isEaofh
                ? halS3UploadEaofh(harvBase, relPath, uploadProgressCb)
                : halS3UploadFile(path, relPath, uploadProgressCb);

            g_tlsActive = false;  // re-enable +++ after upload
            g_uploadingMb = 0.0f; g_uploadBaseMb = 0.0f;

            if (!ur.success) {
                cdc_printf("Upload failed for %s: %s — retrying in 30s\r\n", relPath, ur.error);
                log_write("Upload FAIL: %s (%s)", relPath, ur.error);
                vTaskDelay(pdMS_TO_TICKS(30000)); continue;
            }
            log_write("Uploaded %s %.0f KB/s (%s)", relPath, ur.kbps, ur.error);

            // .eaofh: advance the DSU cookie to the confirmed S3 high-water-mark.
            // halS3UploadEaofh updates the manifest + NVS mfst/hwm but does NOT
            // write the cookie — the firmware does, so the aircraft DSU resumes
            // from the right flight. Idempotent on a manifest-skip.
            if (isEaofh && eaSerial[0]) {
                uint32_t hwm = 0;
                nvs_handle_t nh;
                if (nvs_open("mfst", NVS_READONLY, &nh) == ESP_OK) {
                    nvs_get_u32(nh, "hwm", &hwm); nvs_close(nh);
                }
                if (hwm > 0) {
                    uint8_t newCookie[78];
                    buildDsuCookie(eaSerial, hwm, newCookie);
                    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
                    write_cookie_to_dsu(newCookie, 78);
                    xSemaphoreGive(g_sd_mutex);
                    log_write("Cookie updated: %s flight %lu", eaSerial, (unsigned long)hwm);
                }
            }

            // Delete the uploaded file + remove the subfolder if now empty.
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            markFileUploaded(harvBase, relPath);
            xSemaphoreGive(g_sd_mutex);

            ESP_LOGI(TAG, "Uploaded & deleted: %s", relPath);
            if (g_filesQueued > 0) g_filesQueued--;
            g_filesUploaded++;
            uploadedThisPass++;
            g_mbUploaded += fileMb;
            if (g_mbQueued >= fileMb) g_mbQueued -= fileMb; else g_mbQueued = 0.0f;
        }
        if (g_filesQueued > 0 && !g_harvesting) {
            // The scan found no uploadable files yet the queue counter is non-zero — a
            // STALE count (e.g. orphaned from a prior session). Reset it: a non-zero
            // g_filesQueued keeps `uploading` true forever, which forces logInterval=0
            // and SUPPRESSES the periodic S3 log append — so the session log never grows
            // past its first chunk (TEST 18). An empty scan means /upload is truly drained.
            log_write("Upload: scan found no files but q=%u — resetting stale queue counter", g_filesQueued);
            g_filesQueued = 0;
            g_mbQueued = 0.0f;
        }
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
    ESP_LOGI(TAG, "doHarvest: got mutex");

    // Determine harvest source and destination based on partition layout
    const char* harvestSrc = SD_MOUNT;  // default: single partition
    char harvDir[64];
    snprintf(harvDir, sizeof(harvDir), "%s/upload", SD_MOUNT);
    bool dsu_mounted = false;

    if (g_dual_partition) {
        // Dual-partition: harvest from P1 (/dsu), store on P2 (/sdcard/upload)
        // Mount P1 fresh to get current view of DSU-written files
        if (mount_dsu()) {
            harvestSrc = DSU_MOUNT;
            dsu_mounted = true;
            cdc_printf("doHarvest: P1 mounted at %s\r\n", DSU_MOUNT);
            log_write("doHarvest: P1 mounted OK");
        } else {
            cdc_printf("doHarvest: P1 mount FAILED\r\n");
            log_write("doHarvest: P1 mount failed");
        }
    } else {
        // Single partition: reinit FATFS to get fresh view after MSC writes
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
            ESP_LOGI(TAG, "doHarvest: sd reinit failed");
            return;
        }
    }

    // Harvest files using shared harvestFiles() from airbridge_harvest.h
    uint32_t harvestNum = 0;
    g_hal->nvs->get_u32("harvest", "count", &harvestNum);
    harvestNum++;
    g_hal->nvs->set_u32("harvest", "count", harvestNum);
    HarvestResult hr = harvestFiles(harvestSrc, harvDir, (uint16_t)harvestNum, g_compress);
    uint16_t count = hr.count;
    float usedMb = hr.usedMb;

    // Harvest-integrity guard (airbridge_runtime.h harvestLooksIncomplete): we fired
    // on a host write, so verify we recovered roughly what was written SINCE the last
    // harvest (g_hostWrittenMb is a per-boot monotonic counter, so use the delta). If
    // not (data not yet committed to the SD, or a truncated/unreadable file), don't
    // write the cookie or clear the dirty state — log it and retry on the next quiet
    // window rather than silently dropping the flight or advancing the cookie past it.
    static int s_harvestRetries = 0;
    static float s_lastHarvestWrittenMb = 0.0f;
    float wroteDeltaMb = g_hostWrittenMb - s_lastHarvestWrittenMb;
    if (wroteDeltaMb < 0) wroteDeltaMb = g_hostWrittenMb;   // counter reset (reboot) — use absolute
    bool harvestIncomplete = false;
    if (harvestLooksIncomplete((uint32_t)(wroteDeltaMb * 1024.0f), count,
                               (uint32_t)(usedMb * 1024.0f)) && s_harvestRetries < 3) {
        s_harvestRetries++;
        harvestIncomplete = true;
        log_write("HARVEST: incomplete wrote=%.0fKB got=%u/%.0fKB retry=%d/3 (data may not have committed)",
                  wroteDeltaMb * 1024.0f, count, usedMb * 1024.0f, s_harvestRetries);
        log_flush_to_sd();
    } else {
        s_harvestRetries = 0;
        s_lastHarvestWrittenMb = g_hostWrittenMb;  // snapshot only on a completed harvest
    }

    // Write DSU cookie to the partition the DSU reads from
    if (count > 0 && !harvestIncomplete && !g_s3CookieActive && hr.maxFlight > 0 && hr.dsuSerial[0]) {
        uint8_t cookie[78];
        buildDsuCookie(hr.dsuSerial, hr.maxFlight, cookie);
        if (g_dual_partition && dsu_mounted) {
            // Write directly to /dsu (P1 is still mounted)
            char cookiePath[64];
            snprintf(cookiePath, sizeof(cookiePath), "%s/dsuCookie.easdf", DSU_MOUNT);
            FILE* cf = fopen(cookiePath, "wb");
            if (cf) { fwrite(cookie, 1, 78, cf); fclose(cf); }
        } else {
            write_cookie_to_dsu(cookie, 78);
        }
        log_write("Cookie: %s flight %lu", hr.dsuSerial, (unsigned long)hr.maxFlight);
        cdc_printf("Cookie: %s flight %lu\r\n", hr.dsuSerial, (unsigned long)hr.maxFlight);
    }

    // Unified command file (runtime) — the host may have dropped airbridge.cmd on
    // the USB-visible volume; process runtime-safe directives (dump_logs/wifi/s3/
    // reboot) now, without a reboot. harvestSrc is the USB-visible source (/dsu in
    // dual-partition mode, /sdcard single).
    {
        char cmdPath[80], diagDir[80];
        snprintf(cmdPath, sizeof(cmdPath), "%s/%s", harvestSrc, COMMAND_FILE_NAME);
        snprintf(diagDir, sizeof(diagDir), "%s/diag", harvestSrc);
        CmdRunResult cr = run_command_file(cmdPath, diagDir, /*runtimeOnly=*/true);
        if (cr.reboot) {
            airbridge_log("CMD: reboot — restarting after harvest");
            if (dsu_mounted) unmount_dsu();
            xSemaphoreGive(g_sd_mutex);
            g_harvesting = false; g_msc_ejected = false;
            vTaskDelay(pdMS_TO_TICKS(500));
            sd_before_restart(); esp_restart();
        }
    }

    // Unmount P1 if we mounted it
    if (dsu_mounted) unmount_dsu();

    xSemaphoreGive(g_sd_mutex);

    g_filesQueued += count;
    if (count > 0) g_mbQueued += usedMb;

    // Update SD used space (from internal partition)
    {
        const char* drv = g_dual_partition ? "1:" : "0:";
        FATFS* fs;
        DWORD freeClusters;
        if (f_getfree(drv, &freeClusters, &fs) == FR_OK) {
            DWORD totalSectors = (fs->n_fatent - 2) * fs->csize;
            DWORD freeSectors  = freeClusters * fs->csize;
            g_sdUsedMb = (totalSectors - freeSectors) * 512.0f / 1e6f;
        }
    }

    cdc_printf("doHarvest: done %u file(s) (%.1f MB)\r\n", count, usedMb);
    log_write("Harvest: %u file(s), %.1f MB", count, usedMb);
    log_flush_to_sd();

    if (harvestIncomplete) {
        // Leave g_writeDetected set so shouldHarvest re-fires; re-arm the quiet
        // window so the retry waits for the write to commit to the SD.
        g_lastWriteMs = millis();
    } else {
        g_writeDetected = false; g_lastWriteMs = 0;
        g_hostWasConnected = false; g_hostConnected = false;
    }

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

// CLI removed — all configuration via SD magic files.
// Kept for reference: SETWIFI→WIFI_CONFIG, SETS3→S3_CONFIG, SETMODE→ENABLE_CDC,
// OTA→automatic, UPLOAD→automatic, FORMAT→FORMAT_SD, REBOOT→REBOOT file.


// ── Main loop task ──────────────────────────────────────────────────────────
// Independent no-progress watchdog. Takes NO contended locks, so it keeps running
// even when every other task is wedged. If the main loop's heartbeat goes stale
// (>5 min) the device has hard-hung — reboot it. This is the safety net for the
// 9-hour field brick: a task spun holding the SD/log mutex, the main loop blocked,
// and idle tasks kept feeding the ESP task-WDT so it never tripped. Records the
// reboot in NVS (no log mutex — that may be the wedged resource) for the next boot.
static void watchdog_task(void* param) {
    (void)param;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_CHECK_MS));
        uint32_t hb = g_mainLoopHeartbeat;
        if (watchdogShouldReboot(hb, millis(), WATCHDOG_STALL_MS)) {
            uint32_t stallS = (millis() - hb) / 1000;
            ESP_LOGE(TAG, "WATCHDOG: main loop stalled %lus — rebooting", (unsigned long)stallS);
            nvs_handle_t h;
            if (nvs_open("dbg", NVS_READWRITE, &h) == ESP_OK) {
                uint32_t cnt = 0;
                nvs_get_u32(h, "wdt_reboots", &cnt);
                nvs_set_u32(h, "wdt_reboots", cnt + 1);
                nvs_set_u32(h, "wdt_stall_s", stallS);
                nvs_commit(h);
                nvs_close(h);
            }
            esp_restart();
        }
    }
}

// ── Runtime SD health probe + recovery escalation ───────────────────────────
// A card that mounts at boot but fails later (corrupt FAT, SPI flake) must not
// silently wedge the unit. We probe the SD periodically and escalate per
// sdRecoveryAction(): DEGRADE (non-destructive — mark unmounted, surface the
// fault on the OLED, egress logs over cellular from RAM, keep retrying the
// remount) then, only if it stays dead, REFORMAT (destructive — the DSU
// re-sends the lost queue via the cookie). Boot-time mount failure is already
// handled by the deferred format path; this covers the mid-flight case.
#define SD_DEGRADE_AFTER  5    // consecutive failed probes (~10s at 2s cadence)
#define SD_REFORMAT_AFTER 20   // persistent failure (~40s) → destructive last resort

// Record the reformat in NVS (survives the reboot, reported on next boot),
// give the cellular log-egress a brief window, then reboot into the boot-time
// reformat path. Does not return.
static void request_reformat_and_reboot(const char* reason) {
    g_sd_reformatting = true;
    airbridge_log("SD: unrecoverable (%s) — reformatting; DSU re-sends data via cookie", reason);
    doUpdateDisplay();  // show "SD ERROR / reformatting..." before going down
    {
        nvs_handle_t h;
        if (nvs_open("dbg", NVS_READWRITE, &h) == ESP_OK) {
            uint32_t c = 0; nvs_get_u32(h, "sd_fmt_count", &c);
            nvs_set_u32(h, "sd_fmt_count", c + 1);
            nvs_set_u8(h, "sd_fmt_pending", 1);
            nvs_commit(h); nvs_close(h);
        }
    }
    if (g_pppConnected) vTaskDelay(pdMS_TO_TICKS(3000));  // let degraded egress flush the reason
    {
        nvs_handle_t fh;
        if (nvs_open("sys", NVS_READWRITE, &fh) == ESP_OK) {
            nvs_set_u8(fh, "format", 1); nvs_commit(fh); nvs_close(fh);
        }
    }
    unmount_dsu();
    vTaskDelay(pdMS_TO_TICKS(500));
    sd_before_restart();
    esp_restart();
}

static void sd_health_check() {
    static uint32_t lastMs = 0;
    uint32_t now = millis();
    if (now - lastMs < 2000) return;        // ~every 2s
    lastMs = now;
    if (g_card_sectors == 0) return;        // no card at all — boot handles that

    // Don't probe mid-harvest (it holds the SD mutex for a long stretch). The
    // 50ms mutex timeout is the real contention guard against live MSC/upload —
    // if the bus is busy we skip without counting it as a failure. The probe
    // itself is a LIGHT directory stat (a couple of sector reads, like the log
    // flush already does under live MSC), not a full-FAT f_getfree scan.
    if (g_harvesting) return;
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return; // contended → skip, no count

    bool ok;
    if (g_fatfs_mounted) {
        FILINFO fno;
        FRESULT fr = f_stat(g_dual_partition ? "1:/logs" : "0:/logs", &fno);
        // FR_OK or FR_NO_FILE both mean the filesystem responded; a bus/card
        // failure surfaces as FR_DISK_ERR / FR_NOT_READY / FR_INT_ERR.
        ok = (fr == FR_OK || fr == FR_NO_FILE);
    } else {
        // Degraded/unmounted: try to bring the card back (a transient flake
        // recovers here, with no data loss and no reformat).
        ok = sd_mount_fatfs();
        if (ok) {
            g_sd_degraded = false; g_sd_error[0] = '\0';
            airbridge_log("SD: remounted — recovered from runtime failure");
        }
    }
    xSemaphoreGive(g_sd_mutex);

    // Shared health state machine (also run by the emulator).
    static SdHealthState health;
    SdRecoveryAction act = sdHealthUpdate(health, ok, SD_DEGRADE_AFTER, SD_REFORMAT_AFTER);
    if (act == SD_RECOVERY_REFORMAT) {
        request_reformat_and_reboot("runtime SD failure");   // does not return
    } else if (act == SD_RECOVERY_DEGRADE && !g_sd_degraded) {
        g_sd_degraded   = true;
        g_fatfs_mounted = false;   // stop touching the card; log egress falls back to cellular (RAM buffer)
        snprintf(g_sd_error, sizeof(g_sd_error), "runtime SD failure (n=%lu)",
                 (unsigned long)health.consecutiveFailures);
        airbridge_log("SD: runtime failure (n=%lu) — degraded; logging via cellular, retrying remount",
                      (unsigned long)health.consecutiveFailures);
    }
}

static void main_loop_task(void* param) {
    (void)param;

    // USB presentation: 90s after boot (aircraft DSU timing). OTA+cookie checks
    // share the same deadline — they must complete before USB enumerates, so
    // there's no benefit to waiting longer if they're still in flight.
    #define USB_MIN_DELAY_MS 90000
    #define USB_MAX_DELAY_MS 90000
    uint32_t usbPresentMs = millis();

    for (;;) {
        // Heartbeat for the no-progress watchdog. Stamped first thing each
        // iteration: if the loop body wedges (e.g. blocked on a mutex a hung
        // task holds), this stops advancing and watchdog_task reboots us.
        g_mainLoopHeartbeat = millis();

        // Watchdog: restart modem task if it died (init failure OR runtime crash)
        if (g_modem_task == nullptr) {
            log_write("Modem: task died — restarting");
            cdc_printf("Modem: restarting task...\r\n");
            g_modemReady = false;
            g_pppConnected = false;
            g_modemRssi = 99;
            g_modemOp[0] = '\0';
            xTaskCreatePinnedToCore(modemTask, "modem", 16384, nullptr, 2, &g_modem_task, 0);
            vTaskDelay(pdMS_TO_TICKS(30000));  // 30s cooldown for modem cold boot
        }

        // Enable USB MSC: present when pre-USB tasks done OR 90s timeout elapses
        if (!g_sd_ready && g_card_sectors > 0) {
            uint32_t elapsed = millis() - usbPresentMs;
            bool minElapsed = (elapsed >= USB_MIN_DELAY_MS);
            bool maxElapsed = (elapsed >= USB_MAX_DELAY_MS);
            if (maxElapsed || (minElapsed && g_preUsbDone)) {
                // In MSC-only mode we held D+ low at boot to hide the device.
                // Now raise D+ so the host enumerates for the first time.
                if (g_msc_only) {
                    tud_connect();
                }
                g_sd_ready = true;
                log_write("USB: drive presented (after %ds, preUsb=%s)",
                         (int)(elapsed / 1000), g_preUsbDone ? "done" : "timeout");
                cdc_printf("USB: drive ready\r\n");
            }
        }

        // Print buffered harvest log
        if (g_harvest_log[0]) {
            ESP_LOGI(TAG, "%s", g_harvest_log);
            g_harvest_log[0] = '\0';
        }

        uint32_t now = millis();
        if (!g_splashActive && now - g_lastDisplayMs >= DISPLAY_INTERVAL_MS) {
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

            // Update SD used space (only when MSC is ejected to avoid SPI
            // conflict — f_getfree scans the whole FAT, too heavy for live MSC).
            if (g_fatfs_mounted && g_msc_ejected &&
                xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                FATFS* fs;
                DWORD freeClusters;
                if (f_getfree(g_dual_partition ? "1:" : "0:", &freeClusters, &fs) == FR_OK) {
                    DWORD totalSectors = (fs->n_fatent - 2) * fs->csize;
                    DWORD freeSectors  = freeClusters * fs->csize;
                    g_sdUsedMb = (totalSectors - freeSectors) * 512.0f / 1e6f;
                }
                xSemaphoreGive(g_sd_mutex);
            }
            doUpdateDisplay();
        }

        // Runtime SD health: probe the card, escalate to degrade/reformat if it
        // has gone bad mid-flight. Self-throttled (~2s); only probes when safe.
        sd_health_check();

        // Snapshot volatile write timestamp to avoid race with MSC callback
        uint32_t lastWr = g_lastWriteMs;
        if (shouldHarvest(g_harvesting, g_writeDetected, g_hostWasConnected,
                          lastWr, g_lastHarvestMs, g_harvestCoolMs, now, g_bootHarvestPending)) {
            if (g_bootHarvestPending)
                log_write("Harvest trigger: boot scan (no quiet window)");
            else
                log_write("Harvest trigger: %.1fKB, %us idle", g_hostWrittenMb * 1024.0f, (now - lastWr) / 1000);
            g_bootHarvestPending = false;
            if (g_harvest_task) xTaskNotifyGive(g_harvest_task);
        }
        // Periodic STATUS log (every 60s) — replaces CLI STATUS command
        {
            static uint32_t lastStatusMs = 0;
            if (now - lastStatusMs >= 60000) {
                lastStatusMs = now;
                airbridge_log("STATUS fw=%s device=%s net=%s rssi=%d rsrp=%d sinr=%d lat=%.0fms(%lus) up=%.0fKB/s q=%u upn=%u mbq=%.1f mbup=%.1f heap=%lu",
                    FW_VERSION, g_deviceId,
                    g_pppConnected ? "ppp" : (g_netConnected ? "wifi" : "none"),
                    g_modemRssi, g_modemRsrp, g_modemSinr,
                    linkLatencyNow(), (unsigned long)(g_linkOkMs ? (now - g_linkOkMs) / 1000 : 0),
                    g_lastUploadKBps,
                    g_filesQueued, g_filesUploaded,
                    g_mbQueued, g_mbUploaded,
                    (unsigned long)esp_get_free_heap_size());
            }
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
    airbridge_log("AirBridge fw=%s heap=%lu", FW_VERSION, (unsigned long)esp_get_free_heap_size());

    // Surface a prior watchdog-forced reboot (set by watchdog_task before restart).
    {
        nvs_handle_t h;
        if (nvs_open("dbg", NVS_READWRITE, &h) == ESP_OK) {
            uint32_t cnt = 0, stallS = 0;
            nvs_get_u32(h, "wdt_reboots", &cnt);
            nvs_get_u32(h, "wdt_stall_s", &stallS);
            if (stallS > 0) {
                airbridge_log("WATCHDOG: previous boot force-rebooted after %lus stall (total=%lu)",
                              (unsigned long)stallS, (unsigned long)cnt);
                nvs_set_u32(h, "wdt_stall_s", 0);  // clear so it's reported once
                nvs_commit(h);
            }
            // Runtime SD-reformat report. The RAM ring buffer is lost on the
            // reboot, so the reformat event is recorded in NVS and surfaced here
            // — it egresses to S3 once PPP comes up post-reformat, so the data
            // loss is visible remotely even if the link was down at the time.
            uint8_t sdFmt = 0; uint32_t sdFmtCnt = 0;
            nvs_get_u8(h, "sd_fmt_pending", &sdFmt);
            nvs_get_u32(h, "sd_fmt_count", &sdFmtCnt);
            if (sdFmt) {
                airbridge_log("SD: previous boot reformatted after a runtime card failure "
                              "(total=%lu) — DSU re-sends harvested data via the cookie",
                              (unsigned long)sdFmtCnt);
                nvs_set_u8(h, "sd_fmt_pending", 0);  // report once
                nvs_commit(h);
            }
            nvs_close(h);
        }
    }

    // ── HAL initialization ─────────────────────────────────────────────
    static Esp32Display  s_display;
    static Esp32Clock    s_clock;
    static Esp32Nvs      s_nvs;
    static Esp32Filesys  s_filesys;
    static Esp32Network  s_network;
    static HAL           s_hal = { &s_display, &s_clock, &s_nvs, &s_filesys, &s_network, nullptr };
    g_hal = &s_hal;
    // uart is nullptr — mdm_* helpers fall back to raw ESP-IDF uart_* calls

    // Feed the OLED connection-quality bars from the upload path's presign RTT too —
    // not just the 60s log-append probe, which starves during a long upload and let the
    // bars drop to 0 mid-transfer. Now every per-file presign refreshes the sample.
    g_noteApiRttMs = noteLinkLatency;

    g_sd_mutex = xSemaphoreCreateMutex();

    // ── Session counter (always increments) + crash-loop detection ──────
    {
        nvs_handle_t h;
        if (nvs_open("dbg", NVS_READWRITE, &h) == ESP_OK) {
            // Session counter — monotonic, never reset (for log file naming)
            uint32_t session = 0;
            nvs_get_u32(h, "session", &session);
            session++;
            nvs_set_u32(h, "session", session);

            // Crash-loop counter — reset on successful boot
            uint32_t boots = 0;
            nvs_get_u32(h, "boots", &boots);
            boots++;
            nvs_set_u32(h, "boots", boots);
            nvs_commit(h);
            nvs_close(h);
            g_bootCount = session;
            // Set session log filename (monotonic session number)
            snprintf(g_logFileName, sizeof(g_logFileName), "boot_%04lu", (unsigned long)session);

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
            uint8_t mode = 1;  // default: MSC-only; drop ENABLE_CDC on SD for debug
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
            // Device ID from MAC.
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            char macStr[16];
            snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            char curId[40] = "";
            size_t idLen = sizeof(curId);
            bool haveId = (nvs_get_str(h, "device_id", curId, &idLen) == ESP_OK && curId[0]);
#ifdef ALLOW_CDC_PERSIST
            // E2E build (test-only): force a TEST_-prefixed device id so ALL uploads
            // (logs, files, manifest) route to the test bucket via the backend's
            // _pick_bucket. Overwrites any production id while this firmware is flashed.
            if (strncmp(curId, "TEST_", 5) != 0) {
                char id[40];
                snprintf(id, sizeof(id), "TEST_%s", macStr);
                nvs_set_str(h, "device_id", id);
                nvs_commit(h);
                ESP_LOGW(TAG, "E2E build: device_id forced to %s (test-bucket routing)", id);
            }
#else
            // Production: ensure the real MAC id; strip any leftover TEST_ identity
            // from a prior e2e build so routing returns to the production bucket.
            if (!haveId || strncmp(curId, "TEST_", 5) == 0) {
                nvs_set_str(h, "device_id", macStr);
                nvs_commit(h);
                ESP_LOGI(TAG, "Device ID: %s", macStr);
            }
#endif
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

    // Create logs directory on SD for per-session log files
    if (g_fatfs_mounted) {
        mkdir("/sdcard/logs", 0775);
    }

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

    // ── P1 magic files (accessible via USB MSC even when P2 FATFS is down) ──
    check_p1_magic();

    // ── SD magic file: USB mode switch ──────────────────────────────────
    // Drop ENABLE_CDC on SD to temporarily enable CDC+MSC for this boot.
    // File is deleted after processing. NVS stays MSC-only so the next boot
    // without the file reverts to production mode automatically.
    if (g_fatfs_mounted) {
        char path[64];
        snprintf(path, sizeof(path), "%s/ENABLE_CDC", SD_MOUNT);
        if (access(path, F_OK) == 0) {
            ESP_LOGW(TAG, "SD: ENABLE_CDC found — CDC+MSC for this boot");
            disp("USB Mode", "CDC+MSC (temp)");
            g_msc_only = false;  // enable CDC for this boot only — NVS unchanged
            remove(path);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
#ifdef ALLOW_CDC_PERSIST
        // CDC_PERSIST on P2 — persistent CDC+MSC, NOT removed (re-read every boot).
        snprintf(path, sizeof(path), "%s/%s", SD_MOUNT, CDC_PERSIST_MAGIC);
        if (access(path, F_OK) == 0) {
            ESP_LOGW(TAG, "SD: CDC_PERSIST found — CDC+MSC (persistent until removed)");
            disp("USB Mode", "CDC (persist)");
            g_msc_only = false;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
#endif
        // ── Unified command file on P2 (airbridge.cmd) ──────────────────
        // Mainly relevant on single-partition cards (SD_MOUNT is the USB-visible
        // volume there). Diag dump lands at SD_MOUNT/diag.
        {
            char cmdPath[80], diagDir[80];
            snprintf(cmdPath, sizeof(cmdPath), "%s/%s", SD_MOUNT, COMMAND_FILE_NAME);
            snprintf(diagDir, sizeof(diagDir), "%s/diag", SD_MOUNT);
            CmdRunResult cr = run_command_file(cmdPath, diagDir, /*runtimeOnly=*/false);
            if (cr.format) {
                nvs_handle_t fh;
                if (nvs_open("sys", NVS_READWRITE, &fh) == ESP_OK) {
                    nvs_set_u8(fh, "format", 1); nvs_commit(fh); nvs_close(fh);
                }
                vTaskDelay(pdMS_TO_TICKS(500)); sd_before_restart(); esp_restart();
            }
            if (cr.reboot) {
                vTaskDelay(pdMS_TO_TICKS(500)); sd_before_restart(); esp_restart();
            }
        }

        // ── WIFI_CONFIG — two lines: ssid, password ─────────────────────
        snprintf(path, sizeof(path), "%s/WIFI_CONFIG", SD_MOUNT);
        {
            FILE* wf = fopen(path, "r");
            if (wf) {
                char ssid[64] = "", pass[64] = "";
                if (fgets(ssid, sizeof(ssid), wf)) {
                    ssid[strcspn(ssid, "\r\n")] = '\0';
                    if (fgets(pass, sizeof(pass), wf)) pass[strcspn(pass, "\r\n")] = '\0';
                }
                fclose(wf);
                if (ssid[0]) {
                    char wifiArgs[130];
                    snprintf(wifiArgs, sizeof(wifiArgs), "%s %s", ssid, pass);
                    CliResult r = cliSetWifi(wifiArgs);
                    airbridge_log("SD: WIFI_CONFIG → %s", r.output);
                }
                remove(path);
            }
        }

        // ── S3_CONFIG — two lines: api_host, api_key ────────────────────
        snprintf(path, sizeof(path), "%s/S3_CONFIG", SD_MOUNT);
        {
            FILE* sf = fopen(path, "r");
            if (sf) {
                char host[128] = "", key[64] = "";
                if (fgets(host, sizeof(host), sf)) {
                    host[strcspn(host, "\r\n")] = '\0';
                    if (fgets(key, sizeof(key), sf)) key[strcspn(key, "\r\n")] = '\0';
                }
                fclose(sf);
                if (host[0] && key[0]) {
                    char s3Args[200];
                    snprintf(s3Args, sizeof(s3Args), "%s %s", host, key);
                    CliResult r = cliSetS3(s3Args);
                    airbridge_log("SD: S3_CONFIG → %s", r.output);
                }
                remove(path);
            }
        }

        // ── FORMAT_SD — format SD card ──────────────────────────────────
        snprintf(path, sizeof(path), "%s/FORMAT_SD", SD_MOUNT);
        if (access(path, F_OK) == 0) {
            remove(path);
            airbridge_log("SD: FORMAT_SD found — formatting");
            disp("Formatting SD", "Please wait...");
            // Trigger format (reuses existing FORMAT logic in CLI section)
            // For now, just log — full format requires unmount/remount
            airbridge_log("SD: FORMAT not yet implemented via magic file");
        }

        // ── REBOOT — reboot device ──────────────────────────────────────
        snprintf(path, sizeof(path), "%s/REBOOT", SD_MOUNT);
        if (access(path, F_OK) == 0) {
            remove(path);
            airbridge_log("SD: REBOOT found — rebooting");
            vTaskDelay(pdMS_TO_TICKS(500));
            sd_before_restart();
            esp_restart();
        }
    }

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
            // No RX callback — CDC is log output only (no CLI)
            cdc_cfg.callback_line_coding_changed = cdc_line_coding_callback;
            ESP_ERROR_CHECK(tusb_cdc_acm_init(&cdc_cfg));
        }

        // Avionics (MSC-only) mode: pull D+ low so host sees nothing at all —
        // just power draw — until main_loop_task calls tud_connect() after
        // the presentation delay. In CDC/debug mode, keep USB visible so the
        // developer's serial console works immediately from boot.
        if (g_msc_only) {
            tud_disconnect();
            ESP_LOGI(TAG, "USB: disconnected (hidden from host until delay elapses)");
        }

        if (g_card_sectors > 0) {
            // Delay USB presentation to host by 90s — aircraft DSU needs
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

    // ── FORMAT_SD request (set by the P1/P2 magic-file handler + reboot) ──
    // Honor it by forcing a full reformat, then clear the flag.
    {
        nvs_handle_t fh;
        if (nvs_open("sys", NVS_READONLY, &fh) == ESP_OK) {
            uint8_t fmt = 0;
            esp_err_t e = nvs_get_u8(fh, "format", &fmt);
            nvs_close(fh);
            if (e == ESP_OK && fmt) {
                ESP_LOGW(TAG, "SD: FORMAT_SD requested — forcing full reformat");
                g_needs_full_format = true;
                if (nvs_open("sys", NVS_READWRITE, &fh) == ESP_OK) {
                    nvs_erase_key(fh, "format"); nvs_commit(fh); nvs_close(fh);
                }
            }
        }
    }

    // ── Deferred SD format (needs 16KB stack, can't run in app_main) ────
    // Runs BEFORE task creation so USB hasn't been presented yet.
    ESP_LOGW(TAG, "SD: format flags — full=%d p2=%d dual=%d fatfs=%d card=%p",
             g_needs_full_format, g_p2_needs_format, g_dual_partition, g_fatfs_mounted, g_card);
    if (g_needs_full_format || g_p2_needs_format) {
        ESP_LOGW(TAG, "SD: deferred format needed — spawning format task");
        static volatile bool fmt_done = false;
        xTaskCreatePinnedToCore([](void*) {
            if (g_needs_full_format) {
                g_needs_full_format = false;
                ESP_LOGW(TAG, "FmtTask: full reformat — dual partition");
                cdc_printf("SD: reformatting as dual-partition...\r\n");

                // Unmount without destroying g_card
                if (g_fatfs_mounted) {
                    if (g_dual_partition) { f_mount(NULL, "1:", 0); }
                    else { f_mount(NULL, "0:", 0); }
                    esp_vfs_fat_unregister_path(SD_MOUNT);
                    g_fatfs_mounted = false;
                }

                // Dual-partition format via the shared sequence (airbridge_sd.h::
                // sdFormatDual) — the exact code the native SD tests exercise. The
                // platform hooks below back it with sdmmc + ff_diskio_register; the
                // firmware's offset diskio impls read at g_p1/p2_start_sector, so
                // registerOffset sets those before f_mkfs.
                SdDiskOps ops = {};
                ops.registerRaw    = [](void*){ ff_diskio_register_sdmmc(0, g_card); };
                ops.registerOffset = [](int pdrv, uint32_t start, uint32_t count, void*){
                    if (pdrv == SD_PDRV_P1) { g_p1_start_sector = start;
                                              ff_diskio_register(SD_PDRV_P1, &g_p1_diskio_impl); }
                    else                    { g_p2_start_sector = start; g_p2_sectors = count;
                                              ff_diskio_register(SD_PDRV_P2, &g_p2_diskio_impl); }
                };
                ops.unregister     = [](int pdrv, void*){ ff_diskio_unregister((BYTE)pdrv); };
                ops.readMbr        = [](uint8_t* b, void*){ return sdmmc_read_sectors(g_card, b, 0, 1) == ESP_OK; };
                ops.writeMbr       = [](const uint8_t* b, void*){ return sdmmc_write_sectors(g_card, (void*)b, 0, 1) == ESP_OK; };

                void* work = malloc(4096);
                bool ok = false;
                if (work) {
                    SdFormatResult r = sdFormatDual(ops, g_card_sectors, MSC_MAX_SECTORS,
                                                    16 * 1024, work, 4096);
                    free(work);
                    airbridge_log("FmtTask: fdisk=%d mkfsP1=%d mkfsP2=%d P1@%lu P2@%lu",
                                  r.fdiskFr, r.mkfsP1Fr, r.mkfsP2Fr,
                                  (unsigned long)r.p1Start, (unsigned long)r.p2Start);
                    if (r.ok) {
                        g_p1_start_sector = r.p1Start;
                        g_p2_start_sector = r.p2Start;
                        g_p2_sectors      = r.p2Size;
                        g_dual_partition  = true;
                        ok = true;
                    }
                }
                if (ok) {
                    // Mount P2
                    esp_vfs_fat_conf_t conf = {};
                    conf.base_path = SD_MOUNT; conf.fat_drive = "1:"; conf.max_files = 5;
                    g_p2_fs = nullptr;
                    if (esp_vfs_fat_register_cfg(&conf, &g_p2_fs) == ESP_OK && g_p2_fs &&
                        f_mount(g_p2_fs, "1:", 1) == FR_OK) {
                        g_fatfs_mounted = true;
                        mkdir("/sdcard/upload", 0775);
                        mkdir("/sdcard/logs", 0775);
                        ESP_LOGI(TAG, "FmtTask: dual-partition ready");
                    }
                } else {
                    ESP_LOGE(TAG, "FmtTask: full reformat failed");
                }
            } else if (g_p2_needs_format) {
                g_p2_needs_format = false;
                ESP_LOGW(TAG, "FmtTask: formatting P2 only");
                ff_diskio_register(1, &g_p2_diskio_impl);
                // FM_FAT32|FM_SFD: write FAT VBR at sector 0 of the P2 diskio
                // (= P2's LBA start on the physical card) without creating a
                // nested partition table inside the partition data area.
                MKFS_PARM opt = { .fmt = FM_FAT32 | FM_SFD, .n_fat = 2, .au_size = 16 * 1024 };
                void* work = malloc(4096);
                if (work) {
                    FRESULT fr = f_mkfs("1:", &opt, work, 4096);
                    airbridge_log("FmtTask: P2-only mkfs=%d", fr);
                    free(work);
                    if (fr == FR_OK) {
                        ff_diskio_register_sdmmc(0, g_card);
                        esp_vfs_fat_conf_t conf = {};
                        conf.base_path = SD_MOUNT; conf.fat_drive = "1:"; conf.max_files = 5;
                        g_p2_fs = nullptr;
                        if (esp_vfs_fat_register_cfg(&conf, &g_p2_fs) == ESP_OK && g_p2_fs &&
                            f_mount(g_p2_fs, "1:", 1) == FR_OK) {
                            g_fatfs_mounted = true;
                            mkdir("/sdcard/upload", 0775);
                            mkdir("/sdcard/logs", 0775);
                            ESP_LOGI(TAG, "FmtTask: P2 formatted and mounted");
                        }
                    }
                }
            }
            fmt_done = true;
            vTaskDelete(nullptr);
        }, "sd_fmt", 16384, nullptr, 5, nullptr, 1);  // high priority, 16KB stack

        // Wait for format to complete (blocks app_main — no USB yet)
        while (!fmt_done) vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "SD: deferred format complete");
    }

    // ── Create tasks ────────────────────────────────────────────────────
    // 32KB stack: the shared upload path (halS3UploadEaofh → halS3UploadFile →
    // s3ApiGetViaHal) nests several multi-KB frames (s3Path[2500], hdr[2700], …),
    // which overflows the old 16KB stack and crashes mid-upload on hardware.
    xTaskCreatePinnedToCore(uploadTask,    "upload",    32768, nullptr, 1, &g_upload_task,  1);
    xTaskCreatePinnedToCore(harvestTask,   "harvest",   24576, nullptr, 1, &g_harvest_task, 1);  // +8KB headroom for zlib deflate (gzip)
    xTaskCreatePinnedToCore(modemTask,     "modem",     16384, nullptr, 2, &g_modem_task,   0);  // core 0, 16KB stack for reconnection
    xTaskCreatePinnedToCore(main_loop_task, "main_loop", 4096, nullptr, 1, nullptr,         0);
    // High priority + core 0; takes no contended locks so it survives any wedge.
    xTaskCreatePinnedToCore(watchdog_task,  "watchdog",   3072, nullptr, 5, nullptr,        0);

    // ── Scan /upload/ subfolders for leftover files from before last reboot ──
    if (g_fatfs_mounted) {
        char harvBase[64];
        snprintf(harvBase, sizeof(harvBase), "%s/upload", SD_MOUNT);
        DIR* topDir = opendir(harvBase);
        if (topDir) {
            struct dirent* sub;
            while ((sub = readdir(topDir)) != nullptr) {
                if (sub->d_type != DT_DIR || sub->d_name[0] == '.') continue;
                char subPath[96];
                snprintf(subPath, sizeof(subPath), "%s/%s", harvBase, sub->d_name);
                DIR* subDir = opendir(subPath);
                if (!subDir) continue;
                struct dirent* ent;
                while ((ent = readdir(subDir)) != nullptr) {
                    if (ent->d_type == DT_DIR || ent->d_name[0] == '.') continue;
                    char fullpath[192];
                    snprintf(fullpath, sizeof(fullpath), "%s/%s", subPath, ent->d_name);
                    struct stat st;
                    if (stat(fullpath, &st) == 0 && st.st_size > 0) {
                        g_filesQueued++;
                        g_mbQueued += (float)st.st_size / 1e6f;
                    }
                }
                closedir(subDir);
            }
            closedir(topDir);
            if (g_filesQueued > 0) {
                ESP_LOGI(TAG, "Boot: found %u file(s) in /upload/ — notifying upload", g_filesQueued);
                xTaskNotifyGive(g_upload_task);
            }
        }
    }

    // ── Move old boot logs into upload queue ────────────────────────────
    // With dual-partition, harvest scans P1 (DSU) — old logs on P2 won't be
    // found by harvest. Move them directly into /sdcard/upload/NNNN/ so the
    // upload pipeline picks them up without needing a harvest trigger.
    if (g_fatfs_mounted && g_logFileName[0]) {
        DIR* logDir = opendir("/sdcard/logs");
        if (logDir) {
            // Collect old log names first (avoid readdir + rename conflicts)
            char oldLogs[8][48];
            int nOld = 0;
            struct dirent* ent;
            while ((ent = readdir(logDir)) != nullptr && nOld < 8) {
                if (strncmp(ent->d_name, "boot_", 5) != 0) continue;
                const char* dot = strrchr(ent->d_name, '.');
                if (!dot || strcmp(dot, ".log") != 0) continue;
                char session[48];
                size_t nameLen = dot - ent->d_name;
                if (nameLen >= sizeof(session)) continue;
                memcpy(session, ent->d_name, nameLen);
                session[nameLen] = '\0';
                if (strcmp(session, g_logFileName) == 0) continue;
                strlcpy(oldLogs[nOld], ent->d_name, sizeof(oldLogs[0]));
                nOld++;
            }
            closedir(logDir);

            if (nOld > 0) {
                // Create upload subfolder
                uint32_t hnum = 0;
                g_hal->nvs->get_u32("harvest", "count", &hnum);
                hnum++;
                g_hal->nvs->set_u32("harvest", "count", hnum);
                char destDir[80];
                snprintf(destDir, sizeof(destDir), "/sdcard/upload/%04lu", (unsigned long)hnum);
                mkdir("/sdcard/upload", 0775);
                mkdir(destDir, 0775);

                for (int i = 0; i < nOld; i++) {
                    char src[128], dst[128];
                    snprintf(src, sizeof(src), "/sdcard/logs/%s", oldLogs[i]);
                    snprintf(dst, sizeof(dst), "%s/%s", destDir, oldLogs[i]);
                    if (rename(src, dst) == 0) {
                        log_write("Moved old log %s to upload queue", oldLogs[i]);
                        g_filesQueued++;
                    }
                }
            }
        }
    }

    // ── Scan for unharvested files (from previous session) ─────────────
    // For dual-partition: scan partition 1 (DSU side) by temp-mounting at /dsu.
    // For single partition: scan /sdcard root as before.
    if (g_fatfs_mounted) {
        const char* scanRoot = SD_MOUNT;
        bool dsu_tmp = false;
        if (g_dual_partition) {
            if (mount_dsu()) {
                scanRoot = DSU_MOUNT;
                dsu_tmp = true;
            }
        }

        if (hasUnharvestedFiles(scanRoot)) {
            log_write("Boot: unharvested files on SD — triggering harvest");
            g_writeDetected = true;
            g_hostWasConnected = true;
            g_bootHarvestPending = true;  // pre-existing files need no quiet window
        }

        if (dsu_tmp) unmount_dsu();
    }

    // ── Boot splash (10s) — uses shared dispBootSplash() ──────────────
    if (g_hal->display->ok()) {
        s3LoadCreds();
        dispBootSplash(FW_VERSION, g_deviceId, g_msc_only ? "MSC" : "CDC+MSC");
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

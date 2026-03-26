// AirBridge ESP32-S3 Firmware — ESP-IDF native port
// USB Mass Storage + SD card harvest + S3 upload + WiFi + captive portal + OLED
//
// Build: cd esp32 && ~/.local/bin/pio run
// Flash: 1200-baud touch on CDC port, then pio run -t upload

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
#include <dirent.h>
#include <sys/stat.h>

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
#include "dhcpserver/dhcpserver.h"

static const char *TAG = "airbridge";

// ── Utility: millis() equivalent ─────────────────────────────────────────────
static inline uint32_t millis() {
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

// ── Pin assignments ──────────────────────────────────────────────────────────
#define PIN_I2C_SCL   7
#define PIN_I2C_SDA   8
#define PIN_SD_CS    10
#define PIN_SD_MOSI  11
#define PIN_SD_MISO  12
#define PIN_SD_SCK   13

// ── Display constants ────────────────────────────────────────────────────────
#define SCREEN_W   128
#define SCREEN_H    64
#define OLED_ADDR  0x3C

// ── SSD1306 OLED driver (raw I2C) ───────────────────────────────────────────
static i2c_master_bus_handle_t g_i2c_bus   = nullptr;
static i2c_master_dev_handle_t g_oled_dev  = nullptr;
static uint8_t g_framebuf[SCREEN_W * SCREEN_H / 8];  // 1024 bytes
static bool g_oled_ok = false;

static void oled_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd};  // Co=0, D/C#=0 (command)
    i2c_master_transmit(g_oled_dev, buf, 2, 100);
}

static void oled_init() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = (gpio_num_t)PIN_I2C_SDA;
    bus_cfg.scl_io_num = (gpio_num_t)PIN_I2C_SCL;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    if (i2c_new_master_bus(&bus_cfg, &g_i2c_bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = OLED_ADDR;
    dev_cfg.scl_speed_hz = 400000;

    if (i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &g_oled_dev) != ESP_OK) {
        ESP_LOGE(TAG, "OLED device add failed");
        return;
    }

    // SSD1306 init sequence
    static const uint8_t init_cmds[] = {
        0xAE,       // display off
        0xD5, 0x80, // set clock div
        0xA8, 0x3F, // multiplex 64
        0xD3, 0x00, // display offset 0
        0x40,       // start line 0
        0x8D, 0x14, // charge pump enable
        0x20, 0x00, // memory mode: horizontal
        0xA1,       // segment remap
        0xC8,       // COM scan dec
        0xDA, 0x12, // COM pins
        0x81, 0xCF, // contrast
        0xD9, 0xF1, // precharge
        0xDB, 0x40, // VCOMH deselect
        0xA4,       // display from RAM
        0xA6,       // normal (not inverted)
        0xAF,       // display on
    };
    for (size_t i = 0; i < sizeof(init_cmds); i++) {
        oled_cmd(init_cmds[i]);
    }
    memset(g_framebuf, 0, sizeof(g_framebuf));
    g_oled_ok = true;
}

static void oled_flush() {
    if (!g_oled_ok) return;
    // Set column and page addresses to cover full screen
    oled_cmd(0x21); oled_cmd(0); oled_cmd(127);  // column 0-127
    oled_cmd(0x22); oled_cmd(0); oled_cmd(7);    // page 0-7

    // Send framebuffer in chunks (I2C max transaction ~1024 + overhead)
    // Each chunk: 0x40 prefix (data), then pixel bytes
    for (int page = 0; page < 8; page++) {
        uint8_t buf[SCREEN_W + 1];
        buf[0] = 0x40;  // Co=0, D/C#=1 (data)
        memcpy(buf + 1, &g_framebuf[page * SCREEN_W], SCREEN_W);
        i2c_master_transmit(g_oled_dev, buf, SCREEN_W + 1, 100);
    }
}

static void oled_clear() {
    memset(g_framebuf, 0, sizeof(g_framebuf));
}

static void oled_pixel(int x, int y, bool on) {
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    if (on) g_framebuf[x + (y / 8) * SCREEN_W] |=  (1 << (y & 7));
    else    g_framebuf[x + (y / 8) * SCREEN_W] &= ~(1 << (y & 7));
}

static void oled_hline(int x0, int x1, int y) {
    for (int x = x0; x <= x1; x++) oled_pixel(x, y, true);
}

static void oled_rect(int x, int y, int w, int h, bool fill) {
    if (fill) {
        for (int j = y; j < y + h; j++)
            for (int i = x; i < x + w; i++)
                oled_pixel(i, j, true);
    } else {
        oled_hline(x, x + w - 1, y);
        oled_hline(x, x + w - 1, y + h - 1);
        for (int j = y; j < y + h; j++) {
            oled_pixel(x, j, true);
            oled_pixel(x + w - 1, j, true);
        }
    }
}

// 5x7 font (ASCII 32-126), stored as 5 columns per character
// Standard Adafruit/GLCD font data
static const uint8_t font5x7[] = {
    0x00,0x00,0x00,0x00,0x00, // space
    0x00,0x00,0x5F,0x00,0x00, // !
    0x00,0x07,0x00,0x07,0x00, // "
    0x14,0x7F,0x14,0x7F,0x14, // #
    0x24,0x2A,0x7F,0x2A,0x12, // $
    0x23,0x13,0x08,0x64,0x62, // %
    0x36,0x49,0x55,0x22,0x50, // &
    0x00,0x05,0x03,0x00,0x00, // '
    0x00,0x1C,0x22,0x41,0x00, // (
    0x00,0x41,0x22,0x1C,0x00, // )
    0x08,0x2A,0x1C,0x2A,0x08, // *
    0x08,0x08,0x3E,0x08,0x08, // +
    0x00,0x50,0x30,0x00,0x00, // ,
    0x08,0x08,0x08,0x08,0x08, // -
    0x00,0x60,0x60,0x00,0x00, // .
    0x20,0x10,0x08,0x04,0x02, // /
    0x3E,0x51,0x49,0x45,0x3E, // 0
    0x00,0x42,0x7F,0x40,0x00, // 1
    0x42,0x61,0x51,0x49,0x46, // 2
    0x21,0x41,0x45,0x4B,0x31, // 3
    0x18,0x14,0x12,0x7F,0x10, // 4
    0x27,0x45,0x45,0x45,0x39, // 5
    0x3C,0x4A,0x49,0x49,0x30, // 6
    0x01,0x71,0x09,0x05,0x03, // 7
    0x36,0x49,0x49,0x49,0x36, // 8
    0x06,0x49,0x49,0x29,0x1E, // 9
    0x00,0x36,0x36,0x00,0x00, // :
    0x00,0x56,0x36,0x00,0x00, // ;
    0x00,0x08,0x14,0x22,0x41, // <
    0x14,0x14,0x14,0x14,0x14, // =
    0x41,0x22,0x14,0x08,0x00, // >
    0x02,0x01,0x51,0x09,0x06, // ?
    0x32,0x49,0x79,0x41,0x3E, // @
    0x7E,0x11,0x11,0x11,0x7E, // A
    0x7F,0x49,0x49,0x49,0x36, // B
    0x3E,0x41,0x41,0x41,0x22, // C
    0x7F,0x41,0x41,0x22,0x1C, // D
    0x7F,0x49,0x49,0x49,0x41, // E
    0x7F,0x09,0x09,0x01,0x01, // F
    0x3E,0x41,0x41,0x51,0x32, // G
    0x7F,0x08,0x08,0x08,0x7F, // H
    0x00,0x41,0x7F,0x41,0x00, // I
    0x20,0x40,0x41,0x3F,0x01, // J
    0x7F,0x08,0x14,0x22,0x41, // K
    0x7F,0x40,0x40,0x40,0x40, // L
    0x7F,0x02,0x04,0x02,0x7F, // M
    0x7F,0x04,0x08,0x10,0x7F, // N
    0x3E,0x41,0x41,0x41,0x3E, // O
    0x7F,0x09,0x09,0x09,0x06, // P
    0x3E,0x41,0x51,0x21,0x5E, // Q
    0x7F,0x09,0x19,0x29,0x46, // R
    0x46,0x49,0x49,0x49,0x31, // S
    0x01,0x01,0x7F,0x01,0x01, // T
    0x3F,0x40,0x40,0x40,0x3F, // U
    0x1F,0x20,0x40,0x20,0x1F, // V
    0x3F,0x40,0x38,0x40,0x3F, // W
    0x63,0x14,0x08,0x14,0x63, // X
    0x07,0x08,0x70,0x08,0x07, // Y
    0x61,0x51,0x49,0x45,0x43, // Z
    0x00,0x00,0x7F,0x41,0x41, // [
    0x02,0x04,0x08,0x10,0x20, // backslash
    0x41,0x41,0x7F,0x00,0x00, // ]
    0x04,0x02,0x01,0x02,0x04, // ^
    0x40,0x40,0x40,0x40,0x40, // _
    0x00,0x01,0x02,0x04,0x00, // `
    0x20,0x54,0x54,0x54,0x78, // a
    0x7F,0x48,0x44,0x44,0x38, // b
    0x38,0x44,0x44,0x44,0x20, // c
    0x38,0x44,0x44,0x48,0x7F, // d
    0x38,0x54,0x54,0x54,0x18, // e
    0x08,0x7E,0x09,0x01,0x02, // f
    0x08,0x14,0x54,0x54,0x3C, // g
    0x7F,0x08,0x04,0x04,0x78, // h
    0x00,0x44,0x7D,0x40,0x00, // i
    0x20,0x40,0x44,0x3D,0x00, // j
    0x00,0x7F,0x10,0x28,0x44, // k
    0x00,0x41,0x7F,0x40,0x00, // l
    0x7C,0x04,0x18,0x04,0x78, // m
    0x7C,0x08,0x04,0x04,0x78, // n
    0x38,0x44,0x44,0x44,0x38, // o
    0x7C,0x14,0x14,0x14,0x08, // p
    0x08,0x14,0x14,0x18,0x7C, // q
    0x7C,0x08,0x04,0x04,0x08, // r
    0x48,0x54,0x54,0x54,0x20, // s
    0x04,0x3F,0x44,0x40,0x20, // t
    0x3C,0x40,0x40,0x20,0x7C, // u
    0x1C,0x20,0x40,0x20,0x1C, // v
    0x3C,0x40,0x30,0x40,0x3C, // w
    0x44,0x28,0x10,0x28,0x44, // x
    0x0C,0x50,0x50,0x50,0x3C, // y
    0x44,0x64,0x54,0x4C,0x44, // z
    0x00,0x08,0x36,0x41,0x00, // {
    0x00,0x00,0x7F,0x00,0x00, // |
    0x00,0x41,0x36,0x08,0x00, // }
    0x10,0x08,0x08,0x10,0x08, // ~
};

static void oled_char(int x, int y, char c, int size) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t* glyph = &font5x7[(c - 32) * 5];
    for (int col = 0; col < 5; col++) {
        uint8_t line = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (line & (1 << row)) {
                if (size == 1) {
                    oled_pixel(x + col, y + row, true);
                } else {
                    oled_rect(x + col * size, y + row * size, size, size, true);
                }
            }
        }
    }
}

static void oled_text(int x, int y, const char* str, int size = 1) {
    int cx = x;
    while (*str) {
        oled_char(cx, y, *str, size);
        cx += 6 * size;  // 5 pixels + 1 space
        str++;
    }
}

static int oled_text_width(const char* str, int size = 1) {
    int len = strlen(str);
    return len > 0 ? len * 6 * size - size : 0;  // remove trailing space
}

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
#define QUIET_WINDOW_MS     30000UL
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
static float    g_sdTotalMb      = 0.0f;
static uint32_t g_lastDisplayMs  = 0;
static uint32_t g_lastHarvestMs  = 0;
static uint32_t g_harvestCoolMs  = 30000;

static TaskHandle_t g_upload_task  = nullptr;
static TaskHandle_t g_harvest_task = nullptr;
static uint32_t     g_card_sectors = 0;

// Deferred harvest log
static char g_harvest_log[512] = "";
static char g_sd_error[128] = "";  // persists SD init errors for STATUS display

// ── WiFi / captive portal ───────────────────────────────────────────────────
#define WIFI_AP_SSID            "AirBridge"
#define WIFI_CONNECT_TIMEOUT_MS  10000UL
#define WIFI_GRACE_MS            60000UL
#define AP_RETRY_MS             300000UL
#define MAX_KNOWN_NETS                  5

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

// ── CDC CLI ─────────────────────────────────────────────────────────────────
static char g_cli_buf[128];
static int  g_cli_len = 0;

// ── Forward declarations ────────────────────────────────────────────────────
static void processCLI(const char* cmd);
static void updateDisplay();
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
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (g_msc_ejected || g_harvesting || !g_sd_ready) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = g_card_sectors;
    *block_size  = 512;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           void *buffer, uint32_t bufsize) {
    (void)lun; (void)offset;
    if (!g_sd_ready || g_harvesting || g_msc_ejected) return -1;
    if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return -1;
    esp_err_t err = sdmmc_read_sectors(g_card, buffer, lba, bufsize / 512);
    xSemaphoreGive(g_sd_mutex);
    if (err == ESP_OK) g_lastIoMs = millis();
    return (err == ESP_OK) ? (int32_t)bufsize : -1;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                            uint8_t *buffer, uint32_t bufsize) {
    (void)lun; (void)offset;
    if (!g_sd_ready || g_harvesting || g_msc_ejected) return -1;
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

// TinyUSB handles TEST_UNIT_READY, START_STOP, READ_CAPACITY, INQUIRY, MODE_SENSE
// as built-in commands. tud_msc_scsi_cb is only for non-built-in SCSI commands.
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                         void *buffer, uint16_t bufsize) {
    (void)lun; (void)buffer; (void)bufsize;

    // Handle PREVENT ALLOW MEDIUM REMOVAL
    if (scsi_cmd[0] == 0x1E) {
        tud_msc_set_sense(lun, 0, 0, 0);
        return 0;
    }

    // Default: unsupported command
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject) {
        g_hostConnected = start;
        if (start) g_hostWasConnected = true;
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

// ── WiFi credential storage (NVS namespace "wifi") ──────────────────────────
struct NetCred { char ssid[33]; char pass[65]; };

static int loadKnownNets(NetCred* out) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return 0;
    int32_t n = 0;
    nvs_get_i32(h, "count", &n);
    if (n > MAX_KNOWN_NETS) n = MAX_KNOWN_NETS;
    for (int i = 0; i < n; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        nvs_get_string(h, ks, out[i].ssid, sizeof(out[i].ssid));
        nvs_get_string(h, kp, out[i].pass, sizeof(out[i].pass));
    }
    nvs_close(h);
    return (int)n;
}

static void saveNetwork(const char* ssid, const char* pass) {
    NetCred nets[MAX_KNOWN_NETS];
    int n = loadKnownNets(nets);
    // Remove existing entry for this SSID
    int j = 0;
    for (int i = 0; i < n; i++)
        if (strcmp(nets[i].ssid, ssid) != 0) nets[j++] = nets[i];
    n = j;
    // Prepend (MRU)
    if (n >= MAX_KNOWN_NETS) n = MAX_KNOWN_NETS - 1;
    memmove(&nets[1], &nets[0], sizeof(NetCred) * n);
    strlcpy(nets[0].ssid, ssid, sizeof(nets[0].ssid));
    strlcpy(nets[0].pass, pass, sizeof(nets[0].pass));
    n++;

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "count", n);
    for (int i = 0; i < n; i++) {
        char ks[8], kp[8];
        snprintf(ks, sizeof(ks), "ssid%d", i);
        snprintf(kp, sizeof(kp), "pass%d", i);
        nvs_set_str(h, ks, nets[i].ssid);
        nvs_set_str(h, kp, nets[i].pass);
    }
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "WiFi: saved '%s' (%d stored)", ssid, n);
}

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

static void wifi_init() {
    g_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    g_sta_netif = esp_netif_create_default_wifi_sta();
    g_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                wifi_event_handler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                wifi_event_handler, nullptr));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // we manage NVS ourselves
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ── WiFi helpers ────────────────────────────────────────────────────────────
static int8_t rssiToBars(int32_t rssi) {
    if (rssi >= -55) return 4;
    if (rssi >= -67) return 3;
    if (rssi >= -80) return 2;
    if (rssi >= -90) return 1;
    return 0;
}

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

// ── URL-decode helper for form POST data ────────────────────────────────────
static std::string url_decode(const char* src, size_t len) {
    std::string out;
    out.reserve(len);
    for (size_t i = 0; i < len; i++) {
        if (src[i] == '%' && i + 2 < len) {
            char hex[3] = {src[i+1], src[i+2], 0};
            out += (char)strtol(hex, nullptr, 16);
            i += 2;
        } else if (src[i] == '+') {
            out += ' ';
        } else {
            out += src[i];
        }
    }
    return out;
}

// Extract a named field from URL-encoded form data
static std::string form_field(const char* body, const char* name) {
    std::string needle = std::string(name) + "=";
    const char* start = strstr(body, needle.c_str());
    if (!start) return "";
    start += needle.length();
    const char* end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);
    return url_decode(start, len);
}

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
    vTaskDelay(pdMS_TO_TICKS(2000));  // let USB/SD settle

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
static void disp(const char* line1, const char* line2) {
    oled_clear();
    oled_text(14, 6, "AirBridge", 2);
    oled_hline(0, 127, 26);
    oled_text(0, 32, line1);
    if (line2) oled_text(0, 48, line2);
    oled_flush();
}

static void _fmtSize(char* buf, size_t len, float mb) {
    if (mb >= 1000.0f)   snprintf(buf, len, "%.1fGB", mb / 1024.0f);
    else if (mb >= 0.1f) snprintf(buf, len, "%.1fMB", mb);
    else                 snprintf(buf, len, "%.1fKB", mb * 1024.0f);
}

static void updateDisplay() {
    oled_clear();

    // Row 0: WiFi SSID + signal bars
    char label[18]; strlcpy(label, g_wifiLabel, sizeof(label));
    oled_text(0, 0, label);

    const int8_t xs[4] = {108,113,118,123}, hs[4] = {2,4,6,8};
    for (int i = 0; i < 4; i++) {
        if (i < g_wifiBars) oled_rect(xs[i], 8-hs[i], 3, hs[i], true);
        else                 oled_rect(xs[i], 8-hs[i], 3, hs[i], false);
    }
    oled_hline(0, 127, 9);

    // Row 12: Status label (big text)
    const char* lbl; int lx;
    if (g_harvesting)                                      { lbl = "HARVEST";    lx = 22; }
    else if (g_lastIoMs && millis() - g_lastIoMs < 2000)   { lbl = "USB ACTIVE"; lx =  4; }
    else if (g_writeDetected)                              { lbl = "USB  IDLE";  lx = 10; }
    else                                                   { lbl = "USB READY";  lx = 10; }
    oled_text(lx, 12, lbl, 2);

    // Row 36: USB storage bar
    char sz[12]; _fmtSize(sz, sizeof(sz), g_hostWrittenMb);
    int sizeW = strlen(sz) * 6;
    int sizeX = 128 - sizeW;
    int barX  = 20;
    int barW  = sizeX - 2 - barX;
    oled_text(0, 36, "USB");
    oled_text(sizeX, 36, sz);
    if (barW > 2) {
        oled_rect(barX, 36, barW, 7, false);
        if (g_sdTotalMb > 0.0f && g_hostWrittenMb >= 0.001f) {
            int fill = (int)(g_hostWrittenMb / g_sdTotalMb * (barW - 2));
            if (fill > 0) oled_rect(barX+1, 37, fill, 5, true);
        }
    }

    // Row 50: Upload progress bar
    {
        char upStr[12], remStr[12];
        strcpy(upStr, "UP"); _fmtSize(upStr + 2, sizeof(upStr) - 2, g_mbUploaded);
        strcpy(remStr, "R"); _fmtSize(remStr + 1, sizeof(remStr) - 1, g_mbQueued);
        int upW  = strlen(upStr) * 6;
        int remW = strlen(remStr) * 6;
        int remX = 128 - remW;
        int ubX  = upW + 2;
        int ubW  = remX - 2 - ubX;
        oled_text(0, 50, upStr);
        if (ubW > 2) {
            oled_rect(ubX, 50, ubW, 7, false);
            float totalMb = g_mbUploaded + g_mbQueued;
            if (totalMb > 0.001f) {
                int fill = (int)(g_mbUploaded / totalMb * (ubW - 2));
                if (fill > 0) oled_rect(ubX+1, 51, fill, 5, true);
            }
        }
        oled_text(remX, 50, remStr);
    }

    oled_flush();
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
        ESP_LOGI(TAG, "S3: no credentials in NVS");
        return false;
    }
    nvs_get_string(h, "api_host", g_apiHost, sizeof(g_apiHost));
    nvs_get_string(h, "api_key",  g_apiKey,  sizeof(g_apiKey));
    nvs_get_string(h, "device_id", g_deviceId, sizeof(g_deviceId));
    nvs_close(h);
    if (!g_apiHost[0] || !g_apiKey[0]) {
        ESP_LOGI(TAG, "S3: no credentials in NVS");
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

    // De-chunk
    std::string body;
    size_t pos = 0;
    while (pos < raw.length()) {
        size_t nl = raw.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string szLine = raw.substr(pos, nl - pos);
        // Trim \r
        while (!szLine.empty() && (szLine.back() == '\r' || szLine.back() == ' '))
            szLine.pop_back();
        unsigned long chunkSz = strtoul(szLine.c_str(), nullptr, 16);
        if (chunkSz == 0) break;
        size_t dataStart = nl + 1;
        if (dataStart + chunkSz <= raw.length())
            body.append(raw, dataStart, chunkSz);
        pos = dataStart + chunkSz + 2;
    }
    return body;
}

// Connect TLS to host:443, returns esp_tls handle or nullptr.
static esp_tls_t* tls_connect(const char* host) {
    esp_tls_cfg_t cfg = {};
    cfg.skip_common_name = true;  // like setInsecure()
    // For pre-signed S3 URLs, we skip cert verification (same as Arduino setInsecure)

    esp_tls_t *tls = esp_tls_init();
    if (!tls) return nullptr;

    if (esp_tls_conn_new_sync(host, strlen(host), 443, &cfg, tls) != 1) {
        ESP_LOGE(TAG, "TLS connect failed to %s", host);
        esp_tls_conn_destroy(tls);
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

// Extract a JSON string value for a given key (no JSON library dependency).
static std::string jsonStr(const std::string& json, const char* key) {
    size_t pos = json.find(key);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) {
        // Check for null
        if (json.find("null", colon + 1) != std::string::npos) return "";
        return "";
    }
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static int jsonInt(const std::string& json, const char* key) {
    size_t pos = json.find(key);
    if (pos == std::string::npos) return -1;
    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return -1;
    return atoi(json.c_str() + colon + 1);
}

// Parse URL into host and path components.
static bool parseUrl(const std::string& url, char* host, size_t hostSz,
                     char* path, size_t pathSz) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    size_t hostStart = schemeEnd + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        strlcpy(host, url.substr(hostStart).c_str(), hostSz);
        strlcpy(path, "/", pathSz);
    } else {
        strlcpy(host, url.substr(hostStart, pathStart - hostStart).c_str(), hostSz);
        strlcpy(path, url.substr(pathStart).c_str(), pathSz);
    }
    return true;
}

// Make an HTTPS GET request to the API Gateway presign endpoint.
static std::string s3ApiGet(const char* queryParams) {
    esp_tls_t* tls = tls_connect(g_apiHost);
    if (!tls) {
        ESP_LOGE(TAG, "S3: TLS connect failed to %s", g_apiHost);
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
        esp_tls_conn_destroy(tls);
        return "";
    }
    std::string resp = httpReadResponse(tls);
    esp_tls_conn_destroy(tls);
    return resp;
}

// Make an HTTPS POST to the API Gateway /complete endpoint.
static bool s3ApiComplete(const char* uploadId, const char* key,
                          const char* partsJson) {
    esp_tls_t* tls = tls_connect(g_apiHost);
    if (!tls) {
        ESP_LOGI(TAG, "S3: TLS connect failed (complete)");
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
        esp_tls_conn_destroy(tls);
        return false;
    }

    std::string resp = httpReadResponse(tls);
    esp_tls_conn_destroy(tls);
    bool ok = resp.find("\"ok\"") != std::string::npos;
    if (!ok) ESP_LOGE(TAG, "S3: complete failed: %.200s", resp.c_str());
    return ok;
}

// Upload a file from /sdcard/harvested/<name> to S3 using pre-signed URLs.
static bool s3UploadFile(const char* name) {
    if (!g_netConnected) { ESP_LOGI(TAG, "S3: no WiFi"); return false; }
    if (!s3LoadCreds()) return false;

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
        ESP_LOGE(TAG, "S3: can't open %s", fpath);
        if (f) fclose(f);
        return false;
    }

    static char s3Path[2500];  // shared, STS tokens are long

    // ── Small file: single pre-signed PUT ────────────────────────────────
    if (fileSize <= S3_CHUNK_SIZE) {
        char query[256];
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s", name, fileSize, g_deviceId);
        std::string resp = s3ApiGet(query);
        std::string url = jsonStr(resp, "\"url\"");
        if (url.empty()) {
            ESP_LOGE(TAG, "S3: presign failed: %.200s", resp.c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        char s3Host[128];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            ESP_LOGI(TAG, "S3: URL parse failed");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        esp_tls_t* tls = tls_connect(s3Host);
        if (!tls) {
            ESP_LOGE(TAG, "S3: TLS connect failed to %s", s3Host);
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
            ESP_LOGI(TAG, "S3: header send failed");
            esp_tls_conn_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        if (!httpStreamChunk(tls, f, fileSize)) {
            ESP_LOGI(TAG, "S3: stream failed");
            esp_tls_conn_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);

        std::string putResp = httpReadResponse(tls);
        esp_tls_conn_destroy(tls);
        float elapsed = (millis() - xfrStart) / 1000.0f;
        ESP_LOGI(TAG, "S3: uploaded '%s' OK (%u bytes, %.0f KB/s)", name, fileSize,
                 elapsed > 0 ? fileSize / 1024.0f / elapsed : 0);
        return true;
    }

    // ── Large file: multipart upload ────────────────────────────────────
    char uploadId[256] = "";
    char s3Key[128] = "";
    uint32_t startPart = 1;
    uint32_t totalParts = 0;

    // Check NVS for interrupted session
    {
        nvs_handle_t h;
        if (nvs_open("s3up", NVS_READONLY, &h) == ESP_OK) {
            char storedName[64] = "";
            nvs_get_string(h, "name", storedName, sizeof(storedName));
            if (strcmp(storedName, name) == 0) {
                nvs_get_string(h, "uid", uploadId, sizeof(uploadId));
                nvs_get_string(h, "key", s3Key, sizeof(s3Key));
                uint32_t p = 1; nvs_get_u32(h, "part", &p); startPart = p;
                uint32_t tp = 0; nvs_get_u32(h, "parts", &tp); totalParts = tp;
            }
            nvs_close(h);
        }
    }

    // Start new multipart upload if no session
    if (!uploadId[0]) {
        char query[256];
        snprintf(query, sizeof(query), "file=%s&size=%u&device=%s", name, fileSize, g_deviceId);
        std::string resp = s3ApiGet(query);

        std::string uid = jsonStr(resp, "\"upload_id\"");
        std::string key = jsonStr(resp, "\"key\"");
        totalParts = jsonInt(resp, "\"parts\"");

        if (uid.empty() || key.empty() || totalParts == 0) {
            ESP_LOGE(TAG, "S3: multipart start failed: %.200s", resp.c_str());
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

        ESP_LOGI(TAG, "S3: multipart started, %u parts, upload_id=%s", totalParts, uploadId);
    } else {
        ESP_LOGI(TAG, "S3: resuming multipart at part %u/%u", startPart, totalParts);
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

        char query[512];
        snprintf(query, sizeof(query), "upload_id=%s&key=%s&part=%u",
                 uploadId, s3Key, partNum);
        std::string resp = s3ApiGet(query);
        std::string url = jsonStr(resp, "\"url\"");
        if (url.empty()) {
            ESP_LOGE(TAG, "S3: presign part %u failed: %.200s", partNum, resp.c_str());
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        char s3Host[128];
        if (!parseUrl(url, s3Host, sizeof(s3Host), s3Path, sizeof(s3Path))) {
            ESP_LOGI(TAG, "S3: URL parse failed");
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }
        ESP_LOGI(TAG, "S3: part %u URL len=%d path=%d", partNum, (int)url.length(), (int)strlen(s3Path));

        esp_tls_t* tls = tls_connect(s3Host);
        if (!tls) {
            ESP_LOGE(TAG, "S3: TLS connect failed to %s (part %u)", s3Host, partNum);
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
            esp_tls_conn_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        if (!httpStreamChunk(tls, f, chunkSize)) {
            ESP_LOGE(TAG, "S3: stream failed at part %u", partNum);
            esp_tls_conn_destroy(tls);
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY); fclose(f); xSemaphoreGive(g_sd_mutex);
            return false;
        }

        char etag[64] = "";
        std::string putResp = httpReadResponse(tls, etag, sizeof(etag));
        esp_tls_conn_destroy(tls);

        if (!etag[0]) {
            ESP_LOGE(TAG, "S3: no ETag for part %u, resp(%d): %.300s",
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
                nvs_commit(h);
                nvs_close(h);
            }
        }

        float elapsed = (millis() - xfrStart) / 1000.0f;
        uint32_t totalSent = offset + chunkSize;
        ESP_LOGI(TAG, "S3: part %u/%u done (%u/%u bytes, %.0f%%, %.0f KB/s)",
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
        ESP_LOGI(TAG, "S3: complete_multipart failed — clearing session");
        s3ClearSession();
        return false;
    }

    s3ClearSession();

    float elapsed = (millis() - xfrStart) / 1000.0f;
    ESP_LOGI(TAG, "S3: uploaded '%s' OK (%u bytes, %u parts, %.0f KB/s)",
             name, fileSize, totalParts,
             elapsed > 0 ? fileSize / 1024.0f / elapsed : 0);
    return true;
}

// ── Skip list ───────────────────────────────────────────────────────────────
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

// ── Upload task ─────────────────────────────────────────────────────────────
static void uploadTask(void* param) {
    (void)param;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

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

            if (!name[0]) { ESP_LOGI(TAG, "Upload: no files in /harvested/"); break; }

            char path[128];
            snprintf(path, sizeof(path), "%s/harvested/%s", SD_MOUNT, name);
            ESP_LOGI(TAG, "Upload: found %s", path);

            // Get file size before upload
            xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
            float fileMb = 0.0f;
            {
                struct stat st;
                if (stat(path, &st) == 0) fileMb = (float)st.st_size / 1e6f;
            }
            xSemaphoreGive(g_sd_mutex);

            // Wait for WiFi
            {
                uint32_t waited = 0;
                while (!g_netConnected && waited < 90000) {
                    if (waited == 0) ESP_LOGI(TAG, "Upload: waiting for WiFi...");
                    vTaskDelay(pdMS_TO_TICKS(2000)); waited += 2000;
                }
            }
            if (!g_netConnected) {
                ESP_LOGI(TAG, "Upload: no WiFi after 90s — will retry in 60s");
                vTaskDelay(pdMS_TO_TICKS(60000)); continue;
            }

            ESP_LOGI(TAG, "Uploading: %s (%.1f MB) heap=%lu min=%lu",
                     name, fileMb,
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size());
            bool uploaded = s3UploadFile(name);
            if (!uploaded) {
                ESP_LOGI(TAG, "Upload failed for %s — retrying in 30s", name);
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
        }
        ESP_LOGI(TAG, "Upload idle — %u uploaded", g_filesUploaded);
    }
}

// ── Harvest ─────────────────────────────────────────────────────────────────
static void doHarvest() {
    ESP_LOGI(TAG, "doHarvest: start");
    g_harvesting = true;
    g_msc_ejected = true;  // tell host the drive was ejected
    ESP_LOGI(TAG, "doHarvest: media ejected");
    vTaskDelay(pdMS_TO_TICKS(500));
    updateDisplay();

    // Take mutex to exclude uploadTask from SD for entire harvest
    xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "doHarvest: got mutex, re-initializing SD");

    // Re-initialize card and remount FATFS to get fresh filesystem view
    bool ok = false;
    for (int i = 0; i < 3 && !ok; i++) {
        ok = sd_reinit_and_mount();
        ESP_LOGI(TAG, "doHarvest: sd reinit attempt %d = %s", i+1, ok ? "OK" : "FAIL");
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

    if (!stack[0].dir) {
        ESP_LOGE(TAG, "doHarvest: can't open root dir");
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
        if (stack[depth].prefix[0])
            snprintf(dstName, sizeof(dstName), "%s__%s", stack[depth].prefix, name);
        else
            strlcpy(dstName, name, sizeof(dstName));

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
    ESP_LOGI(TAG, "doHarvest: done %u file(s) (%.1f MB)", count, usedMb);

    g_writeDetected = false; g_lastWriteMs = 0;
    g_hostWasConnected = false; g_hostConnected = false;
    g_hostWrittenMb = 0.0f;

    g_harvesting = false;
    g_msc_ejected = false;
    g_lastHarvestMs = millis();
    g_harvestCoolMs = (count > 0) ? QUIET_WINDOW_MS : 300000UL;
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
        cdc_printf("CLI: wifi=%s ap=%s files_q=%u files_up=%u mbq=%.2f mbup=%.2f sd=%s fatfs=%s sectors=%lu\r\n",
            ipStr, g_apMode ? "yes" : "no",
            g_filesQueued, g_filesUploaded,
            g_mbQueued, g_mbUploaded,
            g_sd_ready ? "ok" : "FAIL", g_fatfs_mounted ? "ok" : "FAIL",
            (unsigned long)g_card_sectors);
        cdc_printf("CLI: wr_det=%d wr_mb=%.2f host_was=%d last_wr=%lu cool=%lu\r\n",
            g_writeDetected, g_hostWrittenMb, g_hostWasConnected,
            (unsigned long)g_lastWriteMs, (unsigned long)g_harvestCoolMs);
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
        cdc_printf("CLI: formatting SD card — ALL DATA WILL BE LOST\r\n");
        g_harvesting = true;
        g_msc_ejected = true;
        vTaskDelay(pdMS_TO_TICKS(500));

        xSemaphoreTake(g_sd_mutex, portMAX_DELAY);
        // Unmount FATFS, format via mkfs, remount
        if (g_fatfs_mounted) {
            esp_vfs_fat_sdcard_unmount(SD_MOUNT, g_card);
            g_fatfs_mounted = false;
        }
        // Re-init card
        sdmmc_card_init(&g_sd_host, g_card);
        // Format and mount
        esp_vfs_fat_mount_config_t mount_cfg = {};
        mount_cfg.format_if_mount_failed = true;
        mount_cfg.max_files = 5;
        esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &g_sd_host,
                                                  &g_slot_config, &mount_cfg, &g_card);
        if (ret == ESP_OK) {
            g_fatfs_mounted = true;
            g_card_sectors = g_card->csd.capacity;
            g_sd_ready = true;
            cdc_printf("CLI: format OK\r\n");
        } else {
            cdc_printf("CLI: format FAILED\r\n");
        }
        xSemaphoreGive(g_sd_mutex);

        g_harvesting = false;
        g_msc_ejected = false;

    } else if (strcmp(cmd, "REBOOT") == 0) {
        cdc_printf("CLI: rebooting...\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();

    } else {
        cdc_printf("CLI: unknown: '%s'\r\n", cmd);
        cdc_printf("CLI: commands: SETWIFI SETS3 STATUS UPLOAD FORMAT REBOOT\r\n");
    }
}

// ── Main loop task ──────────────────────────────────────────────────────────
static void main_loop_task(void* param) {
    (void)param;

    for (;;) {
        // Print buffered harvest log
        if (g_harvest_log[0]) {
            ESP_LOGI(TAG, "%s", g_harvest_log);
            g_harvest_log[0] = '\0';
        }

        uint32_t now = millis();
        if (g_sd_ready && now - g_lastDisplayMs >= DISPLAY_INTERVAL_MS) {
            g_lastDisplayMs = now;
            updateDisplay();
        }

        if (!g_harvesting && g_writeDetected && g_hostWasConnected &&
            g_lastWriteMs != 0 && (now - g_lastWriteMs) >= QUIET_WINDOW_MS &&
            (now - g_lastHarvestMs) >= g_harvestCoolMs &&
            g_hostWrittenMb > 0.01f) {
            ESP_LOGI(TAG, "Harvest trigger: %.1f KB written, %us idle",
                     g_hostWrittenMb * 1024.0f, (now - g_lastWriteMs) / 1000);
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

            esp_reset_reason_t reason = esp_reset_reason();
            ESP_LOGI(TAG, "Boot #%u  reset_reason=%d  heap=%lu",
                     boots, (int)reason, (unsigned long)esp_get_free_heap_size());

            if (boots > 5 && reason != ESP_RST_POWERON) {
                ESP_LOGW(TAG, "CRASH LOOP DETECTED — pausing 30s for debug");
                // Init display early for crash message
                oled_init();
                disp("CRASH LOOP", "Paused 30s");
                vTaskDelay(pdMS_TO_TICKS(30000));
                nvs_handle_t h2;
                if (nvs_open("dbg", NVS_READWRITE, &h2) == ESP_OK) {
                    nvs_set_u32(h2, "boots", 0);
                    nvs_commit(h2);
                    nvs_close(h2);
                }
            }
        }
    }

    // ── OLED init ───────────────────────────────────────────────────────
    if (!g_oled_ok) oled_init();
    if (!g_oled_ok) {
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

    // FATFS already mounted by sd_init() — no separate mount needed

    // ── TinyUSB init (CDC + MSC) ────────────────────────────────────────
    {
        const tinyusb_config_t tusb_cfg = {
            .device_descriptor = nullptr,       // use default
            .string_descriptor = nullptr,       // use default
            .string_descriptor_count = 0,
            .external_phy = false,
            .configuration_descriptor = nullptr, // use default
            .self_powered = false,
            .vbus_monitor_io = -1,
        };
        ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

        // CDC ACM init
        tinyusb_config_cdcacm_t cdc_cfg = {};
        cdc_cfg.usb_dev = TINYUSB_USBDEV_0;
        cdc_cfg.cdc_port = TINYUSB_CDC_ACM_0;
        cdc_cfg.callback_rx = cdc_rx_callback;
        cdc_cfg.callback_line_coding_changed = cdc_line_coding_callback;
        ESP_ERROR_CHECK(tusb_cdc_acm_init(&cdc_cfg));

        if (g_card_sectors > 0) {
            g_sd_ready = true;
            ESP_LOGI(TAG, "MSC ready: %lu sectors (%.0f MB)",
                     (unsigned long)g_card_sectors, g_sdTotalMb);
            disp("USB drive ready", "");
        } else {
            disp("No SD card", "CLI: FORMAT");
        }
    }

    g_lastDisplayMs = millis();

    // ── WiFi init ───────────────────────────────────────────────────────
    wifi_init();

    // ── Create tasks ────────────────────────────────────────────────────
    xTaskCreatePinnedToCore(uploadTask,    "upload",    16384, nullptr, 1, &g_upload_task,  1);
    xTaskCreatePinnedToCore(harvestTask,   "harvest",   16384, nullptr, 1, &g_harvest_task, 1);
    xTaskCreatePinnedToCore(wifiTask,      "wifi",       8192, nullptr, 1, &g_wifi_task,    1);
    xTaskCreatePinnedToCore(main_loop_task, "main_loop", 4096, nullptr, 1, nullptr,         0);

    // ── Scan /harvested/ for leftover files from before last reboot ─────
    if (g_sd_ready && g_fatfs_mounted) {
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

    ESP_LOGI(TAG, "app_main done — free heap: %lu, min: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size());

    // app_main returns; FreeRTOS continues running our tasks
}

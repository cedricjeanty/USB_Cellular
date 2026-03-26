# Code Architecture

## Single-file firmware: `esp32/src/main.cpp`

Both Arduino and ESP-IDF branches use a single source file (~2500 lines) with
FreeRTOS tasks for concurrent operations.

## FreeRTOS Tasks

| Task | Stack | Core | Purpose |
|------|-------|------|---------|
| `uploadTask` | 16KB | 1 | Upload files from /harvested/ to S3 |
| `harvestTask` | 16KB | 1 | Copy new files from SD root to /harvested/ |
| `wifiTask` | 8KB | 1 | WiFi STA connection + captive portal AP |
| `main_loop_task` | 4KB | 0 | Display update, harvest trigger, CLI processing |

## SD Card / FATFS / MSC Interaction

The SD card serves THREE roles simultaneously:
1. **USB Mass Storage** — host reads/writes raw sectors via TinyUSB MSC callbacks
2. **Filesystem access** — harvest/upload use FATFS (via VFS on ESP-IDF, SdFat on Arduino)
3. **Raw sector access** — MSC callbacks use `sdmmc_read/write_sectors()` (ESP-IDF) or `sd.card()->readSectors()` (Arduino)

### Critical: SD Mutex (`g_sd_mutex`)

All SD access is guarded by a FreeRTOS mutex:
- MSC callbacks: 100ms timeout (short — can't block USB task)
- Harvest/upload tasks: `portMAX_DELAY` (blocking)
- `g_harvesting` flag additionally blocks MSC callbacks during harvest

### Harvest Flow

1. Detect USB quiet window (30s no writes, >10KB written)
2. Set `g_harvesting=true`, eject MSC media
3. Take SD mutex
4. Unmount + remount FATFS (refresh after MSC writes)
5. Walk root directory, copy new files to `/harvested/`
6. Release mutex, re-insert MSC media, set `g_harvesting=false`
7. Notify upload task

### ESP-IDF SD Architecture

- `esp_vfs_fat_sdspi_mount()` handles SPI device init + card init + FATFS mount
- `sdmmc_read/write_sectors()` for raw MSC access (uses same card handle)
- `esp_vfs_fat_sdcard_unmount()` + `esp_vfs_fat_sdspi_mount()` for harvest remount
- Local `components/esp_tinyusb/` provides custom `tud_msc_read10_cb`/`tud_msc_write10_cb`
  (default esp_tinyusb uses its own storage layer that doesn't track writes)

### Arduino SD Architecture

- SdFat library provides both raw sector access (`sd.card()->readSectors()`) and
  filesystem access (`sd.open()`, `sd.rename()`)
- `sd.begin()` during harvest refreshes the FS cache
- MSC callbacks use Arduino `USBMSC` class wrapping TinyUSB

## WiFi State Machine

Three states: `WS_TRY_KNOWN` → `WS_CONNECTED` ↔ `WS_AP`

- **TRY_KNOWN:** Try saved networks from NVS (10s timeout each)
- **CONNECTED:** Poll connection every 5s, monitor RSSI
- **AP:** Start captive portal after 60s disconnected, retry saved networks every 5min

ESP-IDF uses event-driven WiFi (`esp_event` handlers set EventGroup bits).
Arduino uses polling (`WiFi.status()`).

## Captive Portal

- DNS server redirects all queries to AP IP (192.168.4.1)
- HTTP server serves WiFi config form at any path
- Form POST with SSID/password → test connection → save to NVS
- ESP-IDF: `esp_http_server` + raw UDP socket for DNS
- Arduino: `WebServer` + `DNSServer` libraries

## Display (SSD1306 OLED)

- 128×64 pixels, I2C at 0x3C
- ESP-IDF: raw I2C commands via `i2c_master` driver, custom 5×7 font, 1KB framebuffer
- Arduino: Adafruit SSD1306 + GFX libraries
- 4-page rotating display: WiFi status, upload progress, drive info

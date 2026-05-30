# Code Architecture

## Single-file firmware: `esp32/src/main.cpp`

Both Arduino and ESP-IDF branches use a single source file (~5400 lines). `main.cpp`
holds the ESP-IDF glue + FreeRTOS task wiring; the hardware-independent logic lives in
`esp32/include/airbridge_*.h` (see the File Map in CLAUDE.md). The active branch is
ESP-IDF + cellular; WiFi/captive-portal and the interactive CLI are disabled.

## FreeRTOS Tasks

| Task | Stack | Core | Priority | Purpose |
|------|-------|------|----------|---------|
| `modemTask` | 16KB | 0 | 2 | UART→PPPoS pump, reconnection, log upload |
| `uploadTask` | 16KB | 1 | 1 | OTA check + upload files from /harvested/ to S3 |
| `harvestTask` | 16KB | 1 | 1 | Copy new files from SD root to /harvested/ |
| `main_loop_task` | 4KB | 0 | 1 | Display update, USB presentation delay, modem watchdog |

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

1. Detect USB quiet window (15s no writes — `QUIET_WINDOW_MS`)
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

## WiFi / Captive Portal (disabled on the active branch)

The active ESP-IDF + cellular branch runs **without WiFi** to free heap for PPPoS +
mbedTLS. `wifi_init()` is not called and `wifiTask` is never started; the WiFi state
machine, captive portal, and DNS server in `main.cpp` are dormant. The credential logic
remains extracted in `airbridge_wifi_creds.h` (with native tests) for the WiFi-capable
Arduino fallback branch (`esp32-s3`).

## Display (SSD1306 OLED)

- 128×64 pixels, I2C at 0x3C
- ESP-IDF: raw I2C commands via `i2c_master` driver, custom 5×7 font, 1KB framebuffer
- Arduino: Adafruit SSD1306 + GFX libraries
- 4-page rotating display: WiFi status, upload progress, drive info

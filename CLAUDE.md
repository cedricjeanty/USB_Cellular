# Developer Brief: USB WiFi Airbridge

Two hardware variants exist: a Raspberry Pi Zero 2 W (legacy, being deprecated) and an
ESP32-S3 (active development). Both present a USB mass storage device to a legacy host,
harvest files when idle, and upload via WiFi.

## ESP32-S3 Variant (Active)

**Hardware:** ESP32-S3-DevKitC-1 (4 MB flash, 2 MB PSRAM)

**USB:** TinyUSB mass storage + CDC serial

**Storage:** SD card via SPI (CS=10, MOSI=11, MISO=12, SCK=13)

**Display:** SSD1306 128×64 OLED at I2C GPIO 7 (SCL) / 8 (SDA), addr 0x3C

**Upload:** S3 via pre-signed URLs from a Lambda backend (no AWS creds on device)

**Source:** `esp32/src/main.cpp` (PlatformIO, single-file firmware)

**Two firmware branches:**
- `esp32-s3` — Arduino framework (90 KB/s upload, fallback)
- `esp32-idf` — ESP-IDF native (279 KB/s upload, active development)

### Duty Cycle

1. **Data Collection:** USB MSC presents SD card to legacy host. Host writes files.
2. **Harvest:** After 30s of no USB writes, eject media, remount FATFS, copy new files
   from SD root to `/harvested/`, re-insert media.
3. **Upload:** Upload files from `/harvested/` to S3 via HTTPS PUT (pre-signed URLs).
   Mark uploaded files with `.done__` markers. Small files (<5 MB) use single PUT;
   large files use S3 multipart with NVS-persisted resume state.
4. **WiFi:** Try saved networks, fall back to captive portal AP ("AirBridge") for
   credential provisioning via phone browser.

### Serial CLI

| Command | Description |
|---------|-------------|
| `SETWIFI <ssid> <pass>` | Save a WiFi network |
| `SETS3 <api_host> <api_key>` | Set S3 upload backend |
| `STATUS` | Show WiFi, upload stats, S3 config |
| `UPLOAD` | Trigger upload task manually |
| `FORMAT` | Format SD card (destroys all data) |
| `REBOOT` | Reboot device |

### NVS Namespaces

| Namespace | Keys | Purpose |
|-----------|------|---------|
| `s3` | `api_host`, `api_key`, `device_id` | Upload credentials |
| `s3up` | `name`, `uid`, `key`, `parts`, `part`, `etagN` | Multipart resume state |
| `wifi` | `ssid0`..`ssid4`, `pass0`..`pass4`, `count` | Saved WiFi networks |

## Detailed Documentation

- **[S3 Upload Architecture](docs/upload.md)** — pre-signed URLs, Lambda backend, multipart, NVS resume
- **[Build & Flash](docs/deployment.md)** — PlatformIO build, 1200-baud touch, flashing both branches
- **[Code Architecture](docs/architecture.md)** — FreeRTOS tasks, SD/FATFS/MSC interaction, WiFi state machine
- **[Debugging Tips](docs/debugging.md)** — common issues, ESP-IDF vs Arduino differences, sdkconfig
- **[Legacy Pi Hardware](docs/pi-legacy.md)** — Raspberry Pi variant (being deprecated)
- **[Testing](docs/testing.md)** — pytest suite, marks, fixtures

## File Map

### ESP32-S3
* `esp32/src/main.cpp` — Single-file firmware: USB MSC, harvest, S3 upload, WiFi, OLED, CLI
* `esp32/platformio.ini` — PlatformIO build config (Arduino on esp32-s3, ESP-IDF on esp32-idf)
* `esp32/sdkconfig.defaults` — ESP-IDF branch: lwIP/mbedTLS tuning (32KB TCP buffers)
* `esp32/components/esp_tinyusb/` — ESP-IDF branch: local TinyUSB with custom MSC callbacks
* `lambda/presign.py` — Lambda function for S3 pre-signed URL generation

### Raspberry Pi (legacy)
* `src/airbridge/main.py` — Primary state machine (harvest loop + upload worker thread)
* `src/airbridge/wifi_manager.py` — WiFi status, rssi_to_csq(), upload_ftp/http()
* `src/airbridge/captive_portal.py` — NM hotspot AP + captive portal HTTP server
* `src/airbridge/display_handler.py` — SSD1306 OLED display
* `config.yaml` — FTP server settings, poll interval, wifi_ap section

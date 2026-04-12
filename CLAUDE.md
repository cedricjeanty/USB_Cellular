# Developer Brief: USB WiFi Airbridge

**Development workflow:** If you find a bug, replicate it in the emulator first, then fix it there. If you add a feature, add it to the emulator, and add a test for it. The emulator runs the same shared code as the firmware — fixes and features should go through the extracted headers (`esp32/include/airbridge_*.h`), not be duplicated.

Two hardware variants exist: a Raspberry Pi Zero 2 W (legacy, being deprecated) and an
ESP32-S3 (active development). Both present a USB mass storage device to a legacy host,
harvest files when idle, and upload via cellular.

## ESP32-S3 Variant (Active)

**Hardware:** ESP32-S3-DevKitC-1 (4 MB flash, 2 MB PSRAM)

**USB:** TinyUSB — MSC-only (PID 0x0002, avionics mode) or CDC+MSC (PID 0x0001, debug mode). Mode stored in NVS, set via `SETMODE MSC` / `SETMODE CDC`.

**Cellular:** SIM7600 modem via UART (TX=43, RX=44), PPPoS. Hologram SIM. Max baud 921600.

**Storage:** SD card via SPI (CS=10, MOSI=11, MISO=12, SCK=13)

**Display:** SSD1306 128×64 OLED at I2C GPIO 7 (SCL) / 8 (SDA), addr 0x3C

**Upload:** S3 via pre-signed URLs from a Lambda backend (no AWS creds on device)

**OTA:** Auto-updates via S3. Dual OTA partitions (ota_0 + ota_1, 1.875 MB each). Rollback on crash loop.

**Source:** `esp32/src/main.cpp` (PlatformIO, single-file firmware ~4500 lines)

**Two firmware branches:**
- `esp32-s3` — Arduino framework (90 KB/s upload, fallback)
- `esp32-idf` — ESP-IDF native (50-60 KB/s cellular upload, active development)

### Duty Cycle

1. **Boot:** 60s USB presentation delay (aircraft DSU expects device to appear after power-on). Modem connects PPPoS in parallel. OTA check runs first (with retry on transient errors).
2. **DSU Cookie:** Writes `dsuCookie.easdf` to SD root (tells aircraft where to resume downloading logs). Can be overridden via S3.
3. **Data Collection:** USB MSC presents SD card to legacy host. Host writes files.
4. **Harvest:** After 15s of no USB writes, eject media, remount FATFS, copy new files
   from SD root to `/harvested/`, re-insert media.
5. **Upload:** Upload files from `/harvested/` to S3 via HTTPS PUT (pre-signed URLs).
   Mark uploaded files with `.done__` markers. Small files (<5 MB) use single PUT;
   large files use S3 multipart with NVS-persisted resume state.

### Serial CLI (CDC mode only)

| Command | Description |
|---------|-------------|
| `SETWIFI <ssid> <pass>` | Save a WiFi network |
| `SETS3 <api_host> <api_key>` | Set S3 upload backend |
| `STATUS` | Show WiFi, upload stats, S3 config, firmware version |
| `UPLOAD` | Trigger upload task manually |
| `FORMAT` | Format SD card (destroys all data) |
| `REBOOT` | Reboot device |
| `OTA` | Check for firmware update and apply |
| `MODEM` | Show modem/PPP status |
| `MODEM_START` | Start modem task if stopped |
| `CELLTEST` | Run 10 MB cellular upload speed test |
| `SETMODE CDC/MSC` | Set USB mode (takes effect on reboot) |

### NVS Namespaces

| Namespace | Keys | Purpose |
|-----------|------|---------|
| `s3` | `api_host`, `api_key`, `device_id` | Upload credentials |
| `s3up` | `name`, `uid`, `key`, `parts`, `part`, `etagN`, `retries` | Multipart resume state |
| `wifi` | `ssid0`..`ssid4`, `pass0`..`pass4`, `count` | Saved WiFi networks |
| `ota` | `ota_status`, `fw_ver` | OTA rollback tracking |
| `usb` | `msc_only` | USB mode preference |

### FreeRTOS Tasks

| Task | Stack | Core | Priority | Purpose |
|------|-------|------|----------|---------|
| modem | 16 KB | 0 | 2 | UART→PPPoS pump, reconnection, log upload |
| upload | 16 KB | 1 | 1 | OTA check, S3 file upload |
| harvest | 16 KB | 1 | 1 | SD file harvest from root to /harvested/ |
| main_loop | 4 KB | 0 | 1 | Display, USB delay, modem watchdog |

### Modem Recovery

The modem task handles multiple baud/mode scenarios on boot:
1. Try AT at 115200 (covers cold boot)
2. Try +++ escape at 921600/460800/3M without flow control (modem in PPP data mode after soft reboot)
3. Try +++ at same bauds with hardware flow control
4. CFUN=0/1 radio reset after AT sync
5. Watchdog in main_loop restarts modem task if it dies (30s cooldown)

## Detailed Documentation

- **[S3 Upload Architecture](docs/upload.md)** — pre-signed URLs, Lambda backend, multipart, NVS resume
- **[Build & Flash](docs/deployment.md)** — PlatformIO build, 1200-baud touch, flashing both branches
- **[Code Architecture](docs/architecture.md)** — FreeRTOS tasks, SD/FATFS/MSC interaction, WiFi state machine
- **[Debugging Tips](docs/debugging.md)** — common issues, ESP-IDF vs Arduino differences, sdkconfig
- **[Legacy Pi Hardware](docs/pi-legacy.md)** — Raspberry Pi variant (being deprecated)
- **[Testing](docs/testing.md)** — pytest suite, marks, fixtures

## File Map

### ESP32-S3
* `esp32/src/main.cpp` — Single-file firmware: USB MSC, cellular, harvest, S3 upload, OTA, OLED, CLI
* `esp32/platformio.ini` — PlatformIO build config (Arduino on esp32-s3, ESP-IDF on esp32-idf)
* `esp32/partitions.csv` — Dual OTA partition table (ota_0 + ota_1, 1.875 MB each)
* `esp32/sdkconfig.defaults` — ESP-IDF branch: lwIP/mbedTLS tuning (32KB TCP buffers)
* `esp32/components/esp_tinyusb/` — ESP-IDF branch: local TinyUSB with custom MSC callbacks
* `lambda/presign.py` — Lambda: S3 pre-signed URLs, firmware version check, DSU cookie, OTA download URL
* `scripts/e2e_test.sh` — E2E test suite (7 tests: OTA, upload, power cuts, combos). Requires CoolGear hub.
* `scripts/commission.sh` — Device commissioning (flash, format SD, verify cellular/OTA/USB)
* `scripts/coolgear.py` — CoolGear USB hub power control for automated testing

### Raspberry Pi (legacy)
* `src/airbridge/main.py` — Primary state machine (harvest loop + upload worker thread)
* `src/airbridge/wifi_manager.py` — WiFi status, rssi_to_csq(), upload_ftp/http()
* `src/airbridge/captive_portal.py` — NM hotspot AP + captive portal HTTP server
* `src/airbridge/display_handler.py` — SSD1306 OLED display
* `config.yaml` — FTP server settings, poll interval, wifi_ap section

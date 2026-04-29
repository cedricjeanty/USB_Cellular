# Developer Brief: USB WiFi Airbridge

**Development workflow:** If you find a bug, replicate it in the emulator first, then fix it there. If you add a feature, add it to the emulator, and add a test for it. The emulator runs the same shared code as the firmware — fixes and features should go through the extracted headers (`esp32/include/airbridge_*.h`), not be duplicated.

Two hardware variants exist: a Raspberry Pi Zero 2 W (legacy, being deprecated) and an
ESP32-S3 (active development). Both present a USB mass storage device to a legacy host,
harvest files when idle, and upload via cellular.

## ESP32-S3 Variant (Active)

**Hardware:** ESP32-S3-DevKitC-1 (4 MB flash, 2 MB PSRAM)

**USB:** TinyUSB — MSC-only (PID 0x0002, avionics mode) or CDC+MSC (PID 0x0001, debug mode). Default is MSC-only. Drop `ENABLE_CDC` on SD to temporarily boot CDC+MSC (file is deleted, next boot reverts to MSC-only). In MSC-only mode, D+ is held low at boot via `tud_disconnect()` — host sees nothing (just power draw) until the 60s presentation delay elapses, then `tud_connect()` triggers first enumeration.

**Cellular:** SIM7600 modem via UART (TX=43, RX=44, RTS=1, CTS=2), PPPoS. Hologram SIM. Runs at 3 Mbaud + HW flow control on PCB.

**Storage:** SD card via SPI (CS=10, MOSI=11, MISO=12, SCK=13)

**Display:** SSD1306 128×64 OLED at I2C GPIO 7 (SCL) / 8 (SDA), addr 0x3C

**Upload:** S3 via pre-signed URLs from a Lambda backend (no AWS creds on device)

**OTA:** Auto-updates via S3. Dual OTA partitions (ota_0 + ota_1, 1.875 MB each). Rollback on crash loop.

**Source:** `esp32/src/main.cpp` (PlatformIO, single-file firmware ~4500 lines)

**Two firmware branches:**
- `esp32-s3` — Arduino framework (90 KB/s upload, fallback)
- `esp32-idf` — ESP-IDF native (50-60 KB/s cellular upload, active development)

### Duty Cycle

1. **Boot:** MSC-only mode holds D+ low (USB invisible) for 60s minimum / 120s max.
   Modem connects PPPoS in parallel. OTA check + S3 cookie fetch run first. USB
   `tud_connect()` fires once OTA+cookie complete (min 60s) or timeout (120s).
   Boot scan also checks for unharvested files in root AND subdirectories (e.g. `flightHistory/`).
2. **DSU Cookie:** 78-byte binary (`dsuCookie.easdf`) on SD root tells aircraft DSU
   where to resume downloading flight logs. Written by firmware after harvest via
   `buildDsuCookie()` (EA1E magic, serial, flight BE u32, CRC-16). Can be overridden
   via S3 one-shot: upload to `s3://BUCKET/firmware/dsuCookie.easdf`, device fetches
   and applies on next boot (Lambda deletes after GET).
3. **Data Collection:** USB MSC presents SD card to legacy host. Host writes files
   (typically to `flightHistory/` subdirectory).
4. **Harvest:** After 15s of no USB writes, remount FATFS, recursively copy new files
   from SD root (including subdirectories) to `/harvested/NNNN/`, flattening paths with
   `__` separator.
5. **Upload:** Upload files from `/harvested/` to S3 via HTTPS PUT (pre-signed URLs).
   Small files (<5 MB) use single PUT; large files use S3 multipart with NVS-persisted
   resume state.
6. **Logging:** `airbridge_log()` writes to serial + 8KB RAM ring buffer. Flushed to
   SD (`/sdcard/logs/boot_NNNN.log`) every 30s, uploaded to S3 via Lambda
   `/prod/log/append` endpoint every 60s. Each boot creates a unique session file
   (monotonic NVS counter `dbg/session`).

### Serial / CDC

CDC is **log-only** (no RX callback, no interactive CLI). All configuration is via SD
magic files. CLI code is kept as dead code for reference. STATUS is logged automatically
every 60s via `airbridge_log()`.

### SD Magic Files

Drop any of these on the SD root; firmware processes and deletes them on boot:
| File | Effect |
|------|--------|
| `ENABLE_CDC` | Boot CDC+MSC this once (deleted after processing, no NVS change) |
| `WIFI_CONFIG` | Two lines: ssid, password |
| `S3_CONFIG` | Two lines: api_host, api_key |
| `firmware.bin` | SD-flash: write to OTA partition + reboot |
| `FORMAT_SD` | Format SD as 8GB FAT32 |
| `REBOOT` | Reboot device |

### NVS Namespaces

| Namespace | Keys | Purpose |
|-----------|------|---------|
| `s3` | `api_host`, `api_key`, `device_id` | Upload credentials |
| `s3up` | `name`, `uid`, `key`, `parts`, `part`, `etagN`, `retries` | Multipart resume state |
| `wifi` | `ssid0`..`ssid4`, `pass0`..`pass4`, `count` | Saved WiFi networks |
| `ota` | `ota_status`, `fw_ver` | OTA rollback tracking |
| `usb` | `msc_only` | USB mode preference |
| `dbg` | `session` | Monotonic boot counter for log filenames |
| `harvest` | `count` | Sequential harvest folder counter |

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
* `esp32/include/airbridge_log.h` — Unified logging: ring buffer + serial + SD flush
* `esp32/include/airbridge_harvest.h` — Recursive directory walk, file move to /harvested/NNNN/
* `esp32/include/airbridge_proto.h` — DSU cookie builder, CRC-16, filename parser, chunked decode
* `esp32/include/airbridge_triggers.h` — Harvest trigger logic (15s quiet window)
* `esp32/include/airbridge_utils.h` — JSON helpers, URL encode/decode, version compare, file skip list
* `esp32/emu/main.cpp` — SDL2 emulator (~870 lines): SimModem via PTY, FileNvs, FakeSD
* `lambda/presign.py` — Lambda: S3 pre-signed URLs, firmware version check, DSU cookie, OTA download URL, log append
* `scripts/e2e_unified.sh` — Unified E2E test suite (15 emulator / 12 hardware tests). `--target emulator` or `--target device`.
* `scripts/commission.sh` — Device commissioning (flash, format SD, verify cellular/OTA/USB)
* `scripts/coolgear.py` — CoolGear USB hub power control for automated testing

### Raspberry Pi (legacy)
* `src/airbridge/main.py` — Primary state machine (harvest loop + upload worker thread)
* `src/airbridge/wifi_manager.py` — WiFi status, rssi_to_csq(), upload_ftp/http()
* `src/airbridge/captive_portal.py` — NM hotspot AP + captive portal HTTP server
* `src/airbridge/display_handler.py` — SSD1306 OLED display
* `config.yaml` — FTP server settings, poll interval, wifi_ap section

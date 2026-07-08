# Code Architecture

## Single-file firmware: `esp32/src/main.cpp`

Both Arduino and ESP-IDF branches use a single source file (~5400 lines). `main.cpp`
holds the ESP-IDF glue + FreeRTOS task wiring; the hardware-independent logic lives in
`esp32/include/airbridge_*.h` (see the File Map below). The active branch is
ESP-IDF + cellular; WiFi/captive-portal and the interactive CLI are disabled.

## FreeRTOS Tasks

| Task | Stack | Core | Priority | Purpose |
|------|-------|------|----------|---------|
| `modemTask` | 16 KB | 0 | 2 | UART→PPPoS pump, reconnection, log upload |
| `uploadTask` | 32 KB | 1 | 1 | OTA check, S3 file upload |
| `harvestTask` | 24 KB | 1 | 1 | SD file harvest from root to /harvested/ (+headroom for zlib deflate) |
| `main_loop_task` | 4 KB | 0 | 1 | Display, USB delay, modem watchdog, heartbeat |
| `watchdog_task` | 3 KB | 0 | 5 | No-progress reboot if main loop wedges (docs/resilience.md) |

## NVS Namespaces

| Namespace | Keys | Purpose |
|-----------|------|---------|
| `s3` | `api_host`, `api_key`, `device_id` | Upload credentials |
| `s3up` | `name`, `uid`, `key`, `parts`, `part`, `etagN`, `retries` | Multipart resume state |
| `wifi` | `ssid0`..`ssid4`, `pass0`..`pass4`, `count` | Saved WiFi networks |
| `ota` | `ota_status`, `fw_ver` | OTA rollback tracking |
| `usb` | `msc_only` | USB mode preference |
| `dbg` | `session`, `boots`, `wdt_reboots`, `wdt_stall_s`, `sd_fmt_pending`, `sd_fmt_count` | Boot session counter for log filenames; crash-loop counter; watchdog reboot record; SD-reformat record |
| `harvest` | `count` | Sequential harvest folder counter |

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

## File Map

The ESP32-S3 codebase is organized so that **all hardware-independent logic lives in
`esp32/include/airbridge_*.h` headers** that compile into BOTH the firmware (`main.cpp`)
and the native emulator/unit tests through the HAL (`esp32/include/hal/`). The firmware
glues these headers to ESP-IDF drivers; the emulator/tests glue them to fakes. New
shared logic belongs in a header, not duplicated in `main.cpp` (see root CLAUDE.md and
`esp32/include/CLAUDE.md`).

### ESP32-S3 (esp32-idf branch, active)

```
esp32/
├── src/main.cpp                 # Firmware entry: USB MSC, FreeRTOS task wiring, ESP-IDF glue (~4800 lines)
│
├── include/                     # ── Shared, hardware-independent logic (firmware + emulator + tests) ──
│   ├── DSU protocol & data
│   │   ├── airbridge_proto.h    #   DSU cookie builder, CRC-16, filename parser, chunked decode
│   │   ├── airbridge_harvest.h  #   Recursive dir walk, file move to /harvested/NNNN/, skip list;
│   │   │                        #   opt-in gzip of .eaofh into the queue (compress arg, -DAIRBRIDGE_COMPRESS)
│   │   └── airbridge_compress.h #   Streaming gzip (zlib) for flight logs — ~3x fewer upload bytes
│   ├── Storage
│   │   └── airbridge_sd.h       #   Shared SD service: dual-partition FORMAT sequence (f_fdisk+MBR fixup+
│   │                            #   f_mkfs P1/P2), diskio injected via SdDiskOps (sdmmc / FakeSd)
│   ├── Cellular
│   │   ├── airbridge_modem.h    #   modemAtSync/modemRunInit/modemReconnect (CEREG/CGACT sequence)
│   │   └── ppp_proto.h          #   Minimal PPP (HDLC/LCP/IPCP) — used by the modem simulator
│   ├── Upload
│   │   ├── airbridge_s3.h       #   S3 upload: single PUT + multipart, NVS resume/retry, manifest/delta
│   │   └── airbridge_http.h     #   HTTP response parsing + S3 API calls (presign/complete) via HAL
│   ├── Runtime & UI
│   │   ├── airbridge_log.h      #   Ring buffer + serial + SD flush logging
│   │   ├── airbridge_runtime.h  #   OTA version check, speed calc, STATUS formatting
│   │   ├── airbridge_display.h  #   SSD1306 page rendering (via HAL display)
│   │   ├── airbridge_triggers.h #   Harvest trigger logic (15s quiet window)
│   │   ├── airbridge_commands.h #   Unified airbridge.cmd parse + persist-rewrite + dump_logs (once modifier)
│   │   └── airbridge_utils.h    #   JSON helpers, URL encode/decode, version compare, skip list
│   ├── airbridge_cli.h          #   CLI command parsing — extracted/tested but DISABLED in firmware
│   ├── airbridge_wifi_creds.h   #   WiFi MRU credential list — extracted/tested but WiFi DISABLED in firmware
│   └── hal/                     #   Hardware Abstraction Layer: g_hal interfaces (nvs/uart/network/filesys/
│                                #   display/clock) + native_impls.h (emulator) + test_impls.h (unit tests)
│
├── emu/main.cpp                 # SDL2 emulator (~1630 lines): wires headers to FakeSD/FileNvs + SimModem PTY
├── include/sim_modem.h          # SIM7600 simulator: AT commands + PPP bridge via pppd
├── include/sim_dsu.h            # Aircraft DSU simulator: reads cookie, emits only un-sent flights;
│                                #   runSession (batched 'd' keypress) + transferFlight (per-flight, atomic
│                                #   .part+rename, rate-limited — used by the emulator's DSU backlog thread)
│
├── lib/fatfs_native/            # Standalone FatFs for native tests (no ESP-IDF/FreeRTOS)
├── test/test_native_*/          # 22 Unity test suites (proto, dsu, harvest, modem, modem_init, http,
│                                #   s3, ppp, nvs, cli, display, runtime, triggers, utils, sd_block, sd_format,
│                                #   commands, command_fetch, compress, fatfsfs, log, net_util)
├── test/fixtures/               # Real .eaofh flight + metrics/ DSU fixtures for sim_dsu
│
├── platformio.ini               # Build config (Arduino on esp32-s3, ESP-IDF on esp32-idf)
├── partitions.csv               # Dual OTA partition table (ota_0 + ota_1, 1.875 MB each)
├── sdkconfig.defaults           # ESP-IDF lwIP/mbedTLS tuning (32KB TCP buffers)
└── components/esp_tinyusb/      # Local TinyUSB fork: custom MSC read10/write10 callbacks
```

### Backend & tooling

```
lambda/presign.py                # S3 pre-signed URLs, fw version check, DSU cookie, OTA URL, log append
scripts/
├── e2e_lib.sh                   # Shared E2E harness library (config + helper defs: start/stop/power_cut,
│                                #   marker, wait_for_* state-triggered waits, get_manifest_hwm, write_dsu_file,
│                                #   seed_manifest*, cleanup_*). Sourced by e2e_unified.sh + flight_cycle_test.sh.
├── e2e_unified.sh               # Unified E2E suite (~25 tests). --target emulator | device.
├── flight_cycle_test.sh         # Emulator soak test: DSU backlog drain over repeated flight cycles
│                                #   (cycles-to-catch-up + backlog area-under-curve; EMU_DSU_* / EMU_CELL_FILE)
├── host_dsu.py                  # Cookie-aware host-side DSU sim (port of sim_dsu.h) for --target device
├── commission.sh                # Device commissioning (flash, format SD, verify cellular/OTA/USB)
├── coolgear.py                  # CoolGear USB hub power control for automated power-cycle tests
├── jtag_flash.py                # Button-free flash recovery (docs/deployment.md)
├── antenna_survey.py            # Parses SURVEY log lines into per-antenna medians (docs/operations.md)
├── test_e2e_esp32.py            # Python E2E harness (hardware)
├── test_upload_resume.py        # Multipart resume regression
└── speed_test.py                # Upload throughput benchmark
```

### Raspberry Pi (legacy, being deprecated)

```
src/airbridge/
├── main.py                      # Primary state machine (harvest loop + upload worker thread)
├── wifi_manager.py              # WiFi status, rssi_to_csq(), upload_ftp/http()
├── captive_portal.py            # NM hotspot AP + captive portal HTTP server
└── display_handler.py           # SSD1306 OLED display
config.yaml                      # FTP server settings, poll interval, wifi_ap section
tests/                           # Pi-era pytest suite (see docs/testing.md)
```

# Developer Brief: USB WiFi Airbridge

**Development workflow:** If you find a bug, replicate it in the emulator first, then fix it there. If you add a feature, add it to the emulator, and add a test for it. The emulator runs the same shared code as the firmware — fixes and features should go through the extracted headers (`esp32/include/airbridge_*.h`), not be duplicated.

Two hardware variants exist: a Raspberry Pi Zero 2 W (legacy, being deprecated) and an
ESP32-S3 (active development). Both present a USB mass storage device to a legacy host,
harvest files when idle, and upload via cellular.

## ESP32-S3 Variant (Active)

**Hardware:** ESP32-S3-DevKitC-1 (4 MB flash, 2 MB PSRAM)

**USB:** TinyUSB — MSC-only (PID 0x0002, avionics mode) or CDC+MSC (PID 0x0001, debug mode). Default is MSC-only. Drop `ENABLE_CDC` on the SD card (P2 internal partition, or P1 USB-visible partition — see SD Magic Files) to temporarily boot CDC+MSC (file is deleted, next boot reverts to MSC-only). In MSC-only mode, D+ is held low at boot via `tud_disconnect()` — host sees nothing (just power draw) until the 90s presentation delay elapses, then `tud_connect()` triggers first enumeration.

**Cellular:** SIM7600 modem via UART (TX=43, RX=44, RTS=1, CTS=2), PPPoS. Hologram SIM. Runs at 3 Mbaud + HW flow control on PCB.

**Storage:** SD card via SPI (CS=10, MOSI=11, MISO=12, SCK=13)

**Display:** SSD1306 128×64 OLED at I2C GPIO 7 (SCL) / 8 (SDA), addr 0x3C

**Upload:** S3 via pre-signed URLs from a Lambda backend (no AWS creds on device)

**OTA:** Auto-updates via S3. Dual OTA partitions (ota_0 + ota_1, 1.875 MB each). Rollback on crash loop.

**Source:** `esp32/src/main.cpp` (PlatformIO, single-file firmware ~5400 lines)

**Two firmware branches:**
- `esp32-s3` — Arduino framework (90 KB/s upload, fallback)
- `esp32-idf` — ESP-IDF native (50-60 KB/s cellular upload, active development)

### Duty Cycle

1. **Boot:** MSC-only mode holds D+ low (USB invisible) for 90s.
   Modem connects PPPoS in parallel. OTA check + S3 cookie fetch run first. USB
   `tud_connect()` fires once OTA+cookie complete OR 90s elapses, whichever first.
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
magic files. The interactive CLI was removed from `main.cpp`; its command-parsing logic
survives (extracted + unit-tested) in `airbridge_cli.h` but is not wired up. STATUS is
logged automatically every 60s via `airbridge_log()`.

### SD Magic Files

In **dual-partition mode** (16GB+ cards), the SD card has two partitions:
- **P1** (8GB, USB-visible via MSC): DSU data — `flightHistory/`, `metrics/`, `dsuCookie.easdf`
- **P2** (rest, firmware-internal): logs, upload queue, magic files processed at boot

Magic files on **P2** (standard path, `SD_MOUNT=/sdcard`):
| File | Effect |
|------|--------|
| `ENABLE_CDC` | Boot CDC+MSC this once (deleted after processing, no NVS change) |
| `CDC_PERSIST` | Boot CDC+MSC **persistently** — re-read every boot, NOT deleted; remove the file to revert to MSC-only. Honored only in `-DALLOW_CDC_PERSIST` builds (the `esp32s3-e2e` env); production OTA builds ignore it. Used by the hardware E2E run. |
| `WIFI_CONFIG` | Two lines: ssid, password |
| `S3_CONFIG` | Two lines: api_host, api_key |
| `firmware.bin` | SD-flash: write to OTA partition + reboot |
| `FORMAT_SD` | Format SD as 8GB FAT32 |
| `REBOOT` | Reboot device |

Magic files on **P1** (fallback path, accessible via USB MSC even when P2 FATFS is down):
| File | Effect |
|------|--------|
| `ENABLE_CDC` | Same as P2 — works even if P2 FATFS fails to mount |
| `CDC_PERSIST` | Same as P2 — persistent CDC+MSC (E2E builds only) |
| `REBOOT` | Same as P2 |

P1 magic files are checked by `check_p1_magic()` at boot after `sd_init()`. This breaks
the catch-22 where P2 FATFS failure made ENABLE_CDC unreachable without physical SD removal.

#### Unified command file (`airbridge.cmd`) — preferred over single-purpose magic files

A single text file `airbridge.cmd` (on P1 — USB-visible — and also honored on P2) holds one
directive per line. Unlike the legacy single-purpose files above, it **persists across boots**
(re-applied every boot, not deleted) so it needn't be re-written each time — *unless* a
directive carries the `once` modifier, which makes it one-shot (run once, then the firmware
rewrites the file with that line removed; the file is deleted when nothing remains). `#`/`;`
comments and blank lines are ignored; verbs are case-insensitive. Shared parse/rewrite logic
lives in `esp32/include/airbridge_commands.h` (unit-tested in `test/test_native_commands`).

| Directive | Effect | Lifetime / build gate |
|-----------|--------|-----------------------|
| `cdc` | Boot CDC+MSC persistently | persistent ⇒ honored only in `-DALLOW_CDC_PERSIST` builds (production stays MSC-only) |
| `cdc once` | Boot CDC+MSC for one boot (= legacy `ENABLE_CDC`) | honored in **all** builds |
| `dump_logs [once]` | Copy P2 `/sdcard/logs/*.log` + the `/sdcard/upload/NNNN/` backlog onto the USB-visible partition at `/diag/` (backlog flattened to `up_<NNNN>_<name>.log`) | all builds; read-only, non-destructive |
| `reboot once` | Reboot | runtime-safe |
| `format_sd once` | Set NVS format flag + reboot (full reformat) | boot-only |
| `wifi <ssid> <pass>` | Save a WiFi network (NVS) | runtime-safe |
| `s3 <api_host> <api_key>` | Save S3 creds (NVS) | runtime-safe |

The file is processed at **boot** (`check_p1_magic()` for P1, the P2 magic block) **and during
the 15s-quiet harvest cycle** (`doHarvest()`, after the P1 fresh-mount). Runtime-safe directives
(`dump_logs`/`wifi`/`s3`/`reboot`) take effect at harvest **without a reboot** — drop the file,
stop writing, wait ~15s, then **replug** the USB drive (no power-cycle) to read `/diag/`.
Directives that need USB re-enumeration (`cdc`) or are destructive (`format_sd`) only apply at
boot. `airbridge.cmd` and `diag/` are in the harvest skip-list so they aren't swept into
`/harvested/`. The legacy single-purpose magic files above remain honored for backward
compatibility (`scripts/hw_flash.sh`, the hardware E2E suite).

**Example `airbridge.cmd`:**
```
# keep CDC serial up across reboots (E2E builds)
cdc
# dump the stored log backlog to the USB drive once, then forget it
dump_logs once
```

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
| main_loop | 4 KB | 0 | 1 | Display, USB delay, modem watchdog, heartbeat |
| watchdog | 3 KB | 0 | 5 | No-progress reboot if main loop wedges |

### No-Progress Watchdog

`main_loop_task` stamps `g_mainLoopHeartbeat = millis()` at the top of every iteration.
An independent high-priority `watchdog_task` (core 0, prio 5, takes **no contended locks**)
reboots the device via `esp_restart()` if the heartbeat goes stale for `WATCHDOG_STALL_MS`
(5 min) — i.e. the main loop hard-wedged. Decision logic is the unit-tested
`watchdogShouldReboot()` (`airbridge_runtime.h`, handles millis() wraparound). The reboot is
recorded in NVS (`dbg/wdt_reboots`, `dbg/wdt_stall_s`) — **not** via `airbridge_log` (the log
mutex may be the wedged resource) — and surfaced on the next boot's `AirBridge fw=` banner as
`WATCHDOG: previous boot force-rebooted after Ns stall`.

**Why this exists:** a field unit (`9C139EF40188`) froze for **9 hours** on "Connecting…"
after an OTA download failed mid-stream on a marginal link: the old firmware's unbounded
`esp_tls` write loop spun on a dead link **holding the SD mutex**, wedging the main loop. It
yielded every 5 ms, so idle tasks kept feeding the ESP task-WDT (`CONFIG_ESP_TASK_WDT_TIMEOUT_S=30`)
and it never tripped. The `netWriteAll()` 30 s write timeout fixes that specific spin; this
watchdog is the defense-in-depth net for any *other* wedge path (a 9-hour brick on an aircraft
is unacceptable). See [[project_diagnostics_command_file]] history.

### OTA Download Resume

`otaDownloadAndFlash()` keeps the `esp_ota` handle across up to 4 attempts and **resumes** a
dropped download with an HTTP `Range: bytes=<received>-` header (`buildRangeGetRequest()` in
`airbridge_http.h`, unit-tested) instead of re-fetching the whole image — so a transient link
hiccup mid-download no longer abandons the firmware (the cause of the unit staying on old fw).
`esp_ota_write` is sequential, so continuing to write the next bytes resumes cleanly; accepts
`200` (full) on attempt 0 and `206` (partial) on resume, and restarts cleanly if a server
ignores `Range` and resends from 0.

### Modem Recovery

**Boot-time AT sync** (modemAtSync / modemRunInit in `airbridge_modem.h`):
1. Try AT at 115200 (covers cold boot)
2. Try +++ escape at 921600/460800/3M without flow control (modem in PPP data mode after soft reboot)
3. Try +++ at same bauds with hardware flow control
4. CFUN=0/1 radio reset after AT sync
5. Watchdog in main_loop restarts modem task if it dies (30s cooldown)

**Mid-session reconnect** (modemReconnect in `airbridge_modem.h`) — after PPP drop:

The correct sequence is **NOT** CFUN=0/1. After a carrier-terminated session the radio
stays registered; CFUN causes real deregistration + forced 30-60s re-attach (worse).

```
ATH                              ← hang up / exit PPP mode
AT+CGACT=0,1                     ← deactivate stale PDP context (critical)
(5s wait for bearer cleanup)
AT+CEREG? / AT+CGREG?            ← check LTE data registration — NOT AT+CREG
AT+CGDCONT=1,"IP","hologram"     ← always re-state APN before redialing
ATD*99#
```

**Key AT command notes for T-Mobile/Hologram LTE SIMs:**
- Use `AT+CEREG?` (LTE/EPS) or `AT+CGREG?` (packet-domain) — **never `AT+CREG?`**
  T-Mobile denies *voice* registration (CREG returns state 3 = denied) but grants
  *LTE data* registration (CEREG returns state 5 = roaming). Checking CREG will always
  time out on T-Mobile/Hologram, causing spurious CFUN resets.
- `AT+CGACT=0,1` before every redial — without it the modem may give CONNECT on a
  stale context where IPCP never completes (no IP assigned).
- CFUN=0/1 is only for genuinely unresponsive modem (not responding to AT at any baud).
- Carrier (T-Mobile/Hologram) terminates sessions every ~4-6 hours — this is normal.
- **Signal metrics:** `modemRunInitPost()` reads `AT+CSQ` (RSSI 0-31) plus, before the PPP
  dial, `AT+CESQ` (RSRP dBm / RSRQ dB, 3GPP indices converted) and `AT+CPSI?` (band + SINR,
  best-effort parse). All in command mode (no `+++`), so no PPP disruption. Logged on the
  `Modem: …` init line and the 60s `STATUS` line (`rsrp=`/`sinr=`). `MODEM_SIG_NA` (9999) =
  unavailable. Parsing is in `airbridge_modem.h` (`parseCesq`/`parseCpsi`), unit-tested;
  `sim_modem.h` simulates both for the emulator/tests.

**Reconnect sequence** — both soft and hard paths send `+++` guard first (escapes PPP
data mode — safe no-op if already in command mode):
- Soft (no CFUN): `+++` → ATH → CGACT=0,1 → 5s → CEREG check → CGDCONT → ATD*99#
- Hard (CFUN): `+++` → ATH → CFUN=0 → 10s → CFUN=1 → 5s → CEREG check → ATD*99#

**Root cause of repeated soft-reconnect failures**: after a reconnect gets CONNECT but
IPCP stalls, the modem stays in PPP data mode. Without the `+++` guard, subsequent soft
reconnects send ATH/CEREG as PPP bytes the modem ignores → 3-minute CEREG timeout each
attempt. The `+++` guard (added fw 20260527160000) fixes this.

**Reconnect backoff** (s_reconnect_failures counter in modem task pump loop):
- Attempts 1-4 (failures 0-3): soft reconnect (no CFUN) — handles carrier termination
- Attempt 5 (failures=4): first full CFUN=0/1 reset
- Attempts 6-9 (failures 5-8): soft
- Attempt 10 (failures=9): second CFUN reset, with progressive backoff
- After 2+ CFUN resets: wait 30s × reset_count (max 300s) before each reset
- Counter resets only when PPP gets IP (g_pppConnected=true), not on CONNECT alone

**pppStale detection**: 90s threshold (was 30s). The 30s LCP echo interval meant one
delayed echo-reply could falsely trigger reconnect on a healthy link.

## Detailed Documentation

- **[S3 Upload Architecture](docs/upload.md)** — pre-signed URLs, Lambda backend, multipart, NVS resume
- **[Build & Flash](docs/deployment.md)** — PlatformIO build, 1200-baud touch, flashing both branches
- **[Code Architecture](docs/architecture.md)** — FreeRTOS tasks, SD/FATFS/MSC interaction
- **[Debugging Tips](docs/debugging.md)** — common issues, ESP-IDF vs Arduino differences, sdkconfig
- **[Legacy Pi Hardware](docs/pi-legacy.md)** — Raspberry Pi variant (being deprecated)
- **[Testing](docs/testing.md)** — native Unity tests, emulator/hardware E2E, legacy Pi pytest
- **[Header De-dup Plan](docs/refactor-header-dedup.md)** — route firmware upload/modem paths through the shared headers (planned)

## File Map

The ESP32-S3 codebase is organized so that **all hardware-independent logic lives in
`esp32/include/airbridge_*.h` headers** that compile into BOTH the firmware (`main.cpp`)
and the native emulator/unit tests through the HAL (`esp32/include/hal/`). The firmware
glues these headers to ESP-IDF drivers; the emulator/tests glue them to fakes. New
shared logic belongs in a header, not duplicated in `main.cpp` (see Developer Brief).

### ESP32-S3 (esp32-idf branch, active)

```
esp32/
├── src/main.cpp                 # Firmware entry: USB MSC, FreeRTOS task wiring, ESP-IDF glue (~5400 lines)
│
├── include/                     # ── Shared, hardware-independent logic (firmware + emulator + tests) ──
│   ├── DSU protocol & data
│   │   ├── airbridge_proto.h    #   DSU cookie builder, CRC-16, filename parser, chunked decode
│   │   └── airbridge_harvest.h  #   Recursive dir walk, file move to /harvested/NNNN/, skip list
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
│   │   └── airbridge_utils.h    #   JSON helpers, URL encode/decode, version compare, skip list
│   ├── airbridge_cli.h          #   CLI command parsing — extracted/tested but DISABLED in firmware
│   ├── airbridge_wifi_creds.h   #   WiFi MRU credential list — extracted/tested but WiFi DISABLED in firmware
│   └── hal/                     #   Hardware Abstraction Layer: g_hal interfaces (nvs/uart/network/filesys/
│                                #   display/clock) + native_impls.h (emulator) + test_impls.h (unit tests)
│
├── emu/main.cpp                 # SDL2 emulator (~1140 lines): wires headers to FakeSD/FileNvs + SimModem PTY
├── include/sim_modem.h          # SIM7600 simulator: AT commands + PPP bridge via pppd
├── include/sim_dsu.h            # Aircraft DSU simulator: reads cookie, emits only un-sent flights
│
├── lib/fatfs_native/            # Standalone FatFs for native tests (no ESP-IDF/FreeRTOS)
├── test/test_native_*/          # 16 Unity test suites (proto, dsu, harvest, modem, modem_init, http,
│                                #   s3, ppp, nvs, cli, display, runtime, triggers, utils, sd_block, sd_format)
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
├── e2e_unified.sh               # Unified E2E suite (~15 tests; consolidated lifecycle + manifest +
│                                #   power-cut tests; state-triggered waits). --target emulator | device.
│                                #   Device runs the SAME consolidated tests via persistent CDC + a
│                                #   serial-log tap (/tmp/dev_e2e.log) + host_dsu.py (see docs/testing.md).
├── host_dsu.py                  # Cookie-aware host-side DSU sim (port of sim_dsu.h): mounts the ESP32
│                                #   USB volume, reads dsuCookie.easdf, emits only un-sent flights. Backs
│                                #   write_dsu_file on --target device. Modes: emit/slice/plant-cookie/
│                                #   read-cookie/partial/--ignore-cookie.
├── commission.sh                # Device commissioning (flash, format SD, verify cellular/OTA/USB)
├── coolgear.py                  # CoolGear USB hub power control for automated power-cycle tests
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

# USB Cellular Airbridge

An ESP32-S3 device that presents a USB mass-storage drive to an aircraft DSU,
harvests the flight logs the DSU writes, and uploads them to S3 over cellular
(SIM7600 + PPPoS, pre-signed URLs from a Lambda backend — no AWS creds on
device). PlatformIO + ESP-IDF, single-file firmware glue + shared headers. A
Raspberry Pi Zero 2 W variant is legacy (docs/pi-legacy.md).

## Development rules

1. **Emulator-first.** If you find a bug, replicate it in the emulator first,
   then fix it there. If you add a feature, add it to the emulator, and add a
   test for it (ADR 0001).
2. **Shared headers.** The emulator runs the same shared code as the firmware —
   all hardware-independent logic lives in `esp32/include/airbridge_*.h`
   (HAL-glued), never duplicated in `main.cpp`, and ships with a native test
   suite. See `esp32/include/CLAUDE.md`.
3. A change is done when the native unit tests pass; behavior changes also run
   the E2E suite against the emulator.
4. **Clean-build before flashing hardware** — incremental pio builds have
   shipped stale binaries twice.
5. Never edit `platformio.ini` or rebuild while an emulator run is in flight —
   the build deletes the running binary.
6. Significant decisions are ADRs in `docs/decisions/` — check there before
   re-deciding anything (CEREG-not-CREG, SD forward recovery, survey mode,
   Safe Mode plane split).

## Commands

- Build firmware: `cd esp32 && ~/.local/bin/pio run -e esp32s3`
- Build emulator: `cd esp32 && ~/.local/bin/pio run -e emulator` → `.pio/build/emulator/program`
- Unit tests (22 suites, no hardware): `cd esp32 && ~/.local/bin/pio test -e native`
- E2E suite: `scripts/e2e_unified.sh --target emulator` (or `device`)
- Flash: docs/deployment.md (1200-baud touch; `scripts/jtag_flash.py` for button-free recovery)

## Hardware (ESP32-S3, active)

- ESP32-S3-DevKitC-1 (4 MB flash, 2 MB PSRAM); SD via SPI (CS=10, MOSI=11,
  MISO=12, SCK=13); SSD1306 128×64 OLED (I2C SCL=7/SDA=8, 0x3C)
- SIM7600 modem: UART TX=43 RX=44 RTS=1 CTS=2, 3 Mbaud + HW flow control,
  Hologram SIM
- USB (TinyUSB): MSC-only (PID 0x0002) in production — D+ held low for the 90s
  boot delay; CDC+MSC (PID 0x0001) for bench, enabled via `cdc`/`cdc once` in
  `airbridge.cmd`. CDC is log-only; there is no interactive CLI.
- OTA: dual partitions (ota_0 + ota_1, 1.875 MB each), rollback on crash loop
- Firmware: `esp32/src/main.cpp` (~4800 lines ESP-IDF glue + task wiring).
  Branches: `esp32-idf` (active, cellular), `esp32-s3` (Arduino/WiFi fallback)

## Duty cycle

1. **Boot**: USB invisible ~90s while the modem connects PPPoS and OTA check +
   S3 cookie fetch run; boot scan finds unharvested files (root + subdirs).
2. **DSU cookie**: 78-byte `dsuCookie.easdf` on SD root tells the aircraft
   where to resume; written after harvest, overridable via S3 one-shot.
3. **Collect**: USB MSC presents the SD to the host (writes `flightHistory/`).
4. **Harvest**: after 15s USB-quiet, remount FATFS and move new files to
   `/harvested/NNNN/` (paths flattened with `__`).
5. **Upload**: HTTPS PUT via pre-signed URLs; <5 MB single PUT, else S3
   multipart with NVS-persisted resume.
6. **Logs/heartbeat**: serial + 8KB RAM ring → SD every 30s → S3 log append
   every 60s; 60s heartbeat POST in all modes (SD-independent).

## Configuration

All device config is via the `airbridge.cmd` file on the SD card (persistent
directives, `once` modifier for one-shots) or legacy single-purpose magic
files; remote units take the same commands over the cellular C2 channel. Full
directive tables: docs/operations.md.

## Documentation map

Do NOT read these up front — read the specific file when a task touches its area:

- `docs/architecture.md` — tasks, SD/FATFS/MSC interaction, NVS namespaces,
  full file map. Read before structural changes.
- `docs/decisions/` — ADRs, one numbered file per decision.
- `docs/modem.md` — AT sync, reconnect sequences, T-Mobile/Hologram gotchas.
  Read before touching modem/PPP code.
- `docs/resilience.md` — watchdog, SD fault recovery, Safe Mode + heartbeat,
  OTA resume. Read before touching failure/recovery paths.
- `docs/operations.md` — airbridge.cmd directives, magic files, dual-partition
  SD layout, antenna survey. Read when configuring or operating a device.
- `docs/testing.md` — unit suites, consolidated E2E, hardware target. Read
  before writing or running tests.
- `docs/upload.md` — S3 presign/multipart/resume. `docs/deployment.md` —
  build/flash/recovery. `docs/debugging.md` — common issues.
- `docs/pi-legacy.md` — legacy Pi variant. `docs/refactor-header-dedup.md` —
  planned de-dup work.
- `esp32/include/CLAUDE.md`, `scripts/CLAUDE.md` — module rules; load
  automatically when working in those directories.

## Agent delegation

Delegation prompts must be self-contained — agents can't see this conversation.

| Agent             | Delegate when...                                          |
| ----------------- | --------------------------------------------------------- |
| `esp32-s3-deployer` | building, flashing, or deploying to the ESP32-S3         |
| `web-researcher`  | external docs needed (ESP-IDF, TinyUSB, SIMCom AT, AWS)    |
| `github-ops`      | any git or GitHub operation                                |

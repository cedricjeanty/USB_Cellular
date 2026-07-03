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

**Source:** `esp32/src/main.cpp` (PlatformIO, single-file firmware ~4800 lines)

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
| `survey` | Antenna signal-survey mode (see below) | boot-only |
| `compress [on\|off]` | gzip `.eaofh` into the upload queue at harvest (~3x fewer bytes over cellular; ROM miniz `tdefl` in PSRAM). Default OFF — the S3 consumer must `gunzip` the objects | runtime-safe (next harvest) |

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
| `dbg` | `session`, `boots`, `wdt_reboots`, `wdt_stall_s`, `sd_fmt_pending`, `sd_fmt_count` | Boot session counter for log filenames; crash-loop counter; watchdog reboot record; SD-reformat record |
| `harvest` | `count` | Sequential harvest folder counter |

### FreeRTOS Tasks

| Task | Stack | Core | Priority | Purpose |
|------|-------|------|----------|---------|
| modem | 16 KB | 0 | 2 | UART→PPPoS pump, reconnection, log upload |
| upload | 32 KB | 1 | 1 | OTA check, S3 file upload |
| harvest | 24 KB | 1 | 1 | SD file harvest from root to /harvested/ (+headroom for zlib deflate) |
| main_loop | 4 KB | 0 | 1 | Display, USB delay, modem watchdog, heartbeat |
| watchdog | 3 KB | 0 | 5 | No-progress reboot if main loop wedges |

### Antenna Signal Survey

For comparing antennas, the `survey` directive puts the unit in a **dedicated measurement
mode**: the modem task registers on the network and then **loops `modemSurveySample()`**
(`AT+CSQ` + `AT+CESQ` + `AT+CPSI` + `AT+COPS?`) every 2 s, logging
`SURVEY N: carrier=… band=… RSSI=… RSRP=… RSRQ=… SINR=…`. It **never dials PPP**, so the AT
polling never collides with an upload (mixing the two is what broke uploads before — once PPP
is up, escaping with `+++` to run AT commands stalls the data session). Survey mode and normal
upload operation are mutually exclusive.

Procedure (bench, with persistent CDC): flash `esp32s3-e2e`, put `cdc` + `survey` in
`airbridge.cmd` on the USB volume, power-cycle, and read the `SURVEY` lines live on
`/dev/ttyACM*`. `scripts/antenna_survey.py` parses them into a live readout + per-antenna
median RSRP/SINR (press Enter to label/summarize an antenna and start the next). Swap antennas
and watch RSRP/SINR settle — no reboot per antenna. Compare on **RSRP** (dBm, coverage/path-loss
— the primary antenna metric) and **SINR** (dB, quality); only compare samples on the same
`band` (the Hologram multi-carrier SIM reselects, e.g. Verizon BAND13 ≈ -96 dBm vs BAND4 ≈ -114
dBm at the same spot). Remove `survey` from `airbridge.cmd` to return to normal operation.
`modemRegisterAndReadSignal()` is shared with `modemRunInitPost()`; `modemSurveySample()` is
unit-tested against `sim_modem.h`.

**Band lock (essential for valid comparisons).** The modem freely reselects band/cell, swinging
RSRP 10+ dB and swamping antenna differences — a real gotcha here: an antenna looked "13 dB
better" only because the modem camped on Verizon Band 5 vs Band 4; locked to the same band all
four antennas read within ~3 dB. Use `survey band=N` to lock LTE band N (`AT+CNMP=38` LTE-only +
`AT+CNBP=0xFFFFFFFFFFFFFFFF,0x<1<<(N-1)>`) so every antenna sees identical RF. Verified on
hardware (`band=5` forces Band 5, `band=4` forces Band 4). **`AT+CNMP`/`AT+CNBP` persist in the
MODEM's NVS** (survive reboot/reflash) — you MUST run `survey band=auto` to restore auto-band
before a unit returns to service. (`AT+CNBP=?` returns ERROR on this SIM7600 firmware — harmless;
the 2-arg set form works.) `scripts/antenna_survey.py` summarizes per antenna.

### Button-free flash / recovery (`scripts/jtag_flash.py`)

Normal button-free flashing is the 1200-baud touch on CDC (`docs/deployment.md`) — but that
needs the firmware to boot far enough to present CDC. If a bad build hangs in early boot it
presents nothing usable, and historically the only recovery was the (often inaccessible) BOOT
button. `scripts/jtag_flash.py` recovers WITHOUT the button: at power-on the chip briefly
exposes its USB-Serial-JTAG (`303a:1001`) before the app switches USB to TinyUSB; a *cold*
esptool misses that <1s window, so the script keeps a **warm (pre-imported) esptool** busy-
waiting and fires `default-reset` the instant the JTAG enumerates — landing in the window and
dropping the chip into stable ROM download mode. It then `erase`s (clears any stale OTA
selector — the cause of one boot-loop) and writes the full image. Uses the CoolGear hub for
power if present, else `--manual` prompts. `--app-bin latest.bin` flashes a prod app over the
env's bootloader/partitions. **Note:** `pio run -t upload` writes only the app slot and can
leave a stale OTA selector → boot a wrong/garbage slot; prefer this script (or `pio run -t
erase` first) when the OTA partition state is uncertain.

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

### SD-Card Fault Robustness

A corrupt/failing SD card must never silently dark a unit, and the firmware must self-recover
without a tech on site. Three layers:

1. **Log even with no filesystem.** Log egress is decoupled from the SD. The `airbridge_log`
   RAM ring buffer (`airbridge_log.h`) is SD-independent; when `!g_fatfs_mounted` but PPP is up,
   the upload task POSTs the ring-buffer snapshot straight to `/prod/log/append` and `consume`s
   exactly what it sent (`airbridge_log_consume`). So a unit with a totally dead card still phones
   home "I'm alive, my SD failed." The two log paths are mutually exclusive on `g_fatfs_mounted`.
2. **Surface it on the OLED.** When the card is present but unmounted (`g_card_sectors>0 &&
   !g_fatfs_mounted`) the display shows an `SD ERROR / recovering…` (or `reformatting…`) overlay —
   covers both a boot-time mount failure and a runtime degrade, auto-clears on remount.
3. **Escalate to reformat (data loss accepted).** A periodic light probe (`sd_health_check`,
   `f_stat` under a 50 ms mutex timeout, ~2 s cadence, skipped mid-harvest) feeds a consecutive-
   failure count into `sdRecoveryAction()` (`airbridge_runtime.h`, unit-tested): **DEGRADE** first
   (non-destructive — mark unmounted, surface the fault, log over cellular, retry the remount; a
   transient flake recovers here with no data loss), then **REFORMAT** only if it stays dead
   (destructive — the DSU re-sends the lost queue via the cookie). The reformat reuses the boot
   format path (sets NVS `sys/format`, reboots) and is recorded in NVS (`dbg/sd_fmt_pending`/
   `sd_fmt_count`) so the event — lost when RAM clears on reboot — is reported on the next boot and
   egresses once PPP is up. There is deliberately **no boot watchdog** for SD faults: a reboot
   can't fix a corrupt FAT, so recovery is forward (reformat), not retry.

The block-level format/mount/corruption logic is exercised by real FatFs against an in-memory
`FakeSd` disk in `test/test_native_sd_block` (MBR/P1/P2 corruption → unmountable → reformat
recovers; benign FSInfo corruption still mounts, no data loss; closed files survive an unclean
restart). Runtime detection while MSC is *continuously* presented is best-effort (the probe runs
when the SD mutex is briefly free); the always-on protection is the boot-time mount-failure →
reformat path, now made visible by layers 1–2.

**Emulating a real SD in the SDL emulator.** `include/hal/fatfs_filesys.h` (`FatFsFilesys`) is an
`IFilesys` backed by real FatFs on the `FakeSd` block device (LFN enabled via
`lib/fatfs_native` + `ffunicode.c`, so the harvest's `__`-flattened long names work). Run the
emulator with **`EMU_SD_BLOCK=1`** and the SD becomes a genuine FAT image (host files in
`./emu_sdcard[_internal]` are imported at boot so the drop-files workflow still seeds it). Then
`B` / `SIGUSR2` / `EMU_SD_CORRUPT_AFTER_MS` injects real FAT corruption and the emulator runs the
same `sdRecoveryAction` escalation — degrade (OLED `SD ERROR`) → reformat → recover — end-to-end.
Default mode (no env var) stays on host directories, so the existing e2e suite is unaffected.
Coverage: `test/test_native_fatfsfs` (the backend in isolation) + `scripts/test_emu_sd_block.sh`
(the lifecycle inside the emulator binary). Note `test_native_sd_block` keeps its **own** vendored
FatFs (LFN=0) and is deliberately decoupled from `lib/fatfs_native` (LFN=1) to avoid an ffconf
collision on the native include path.

### Safe Mode / Crash-Loop Firewall + Heartbeat

A data fault (corrupt SD, bad config, an app-init panic) must never turn the device into an
unreachable brick — once deployed, there's no physical access. The defense splits a **survival
plane** (cellular + the remote command channel + telemetry, all SD-independent) from the
**application plane** (SD harvest / USB MSC / upload). A crash loop degrades to "connected, in
Safe Mode, awaiting orders," never a brick.

- **Crash-loop counter** (`dbg/boots`): bumped at the top of every boot, reset to 0 **only** at
  the main-loop "mark healthy" point (~60 s of stable uptime) or by a deliberate remote fix — so
  it actually counts consecutive boots that never reached stable. (It used to reset every boot,
  so `boots>5` never fired — there was no working crash-loop protection at all.)
- **decideBootMode()** (`airbridge_runtime.h`, unit-tested, pure): `boots>=3 && reset!=POWERON`
  → `SAFE_MODE`, or `OTA_ROLLBACK` if an OTA is pending its first-run confirmation. A clean
  power-on always gets one normal retry (a human power-cycling forces a fresh attempt).
- **Safe Mode** (`g_safe_mode`, set in `app_main` before any SD touch): short-circuits the
  app-plane boot (no `sd_init`/magic/USB/`uploadTask`/`harvestTask`) and starts ONLY `modemTask`
  (cellular + C2 command poll + SD-independent RAM-log egress) + `watchdog_task` + a minimal
  `main_loop_task`. Surfaced on the boot banner, OLED, and heartbeat. **Stays** in Safe Mode
  until a deliberate fix clears the firewall.
- **Recovery, fully remote**: remote `format_sd` resets `dbg/boots` → next boot is NORMAL and
  reformats the SD (clears the corruption); a pending-OTA crash loop auto-rolls-back to the
  previous slot at boot; and a periodic **safe-mode OTA self-heal** lets a fixed firmware merely
  *uploaded to S3* heal a firmware-bug crash loop with no command needed.
- **Always-on heartbeat**: every 60 s in BOTH modes the device POSTs
  `{mode, boots, reset_reason, fw, net, sd_mounted, rssi, rsrp, sinr, heap, uptime_s}` over the
  SD-independent egress path → Lambda `POST/GET /command/heartbeat` → latest-wins
  `heartbeat/{device}.json`. So an operator can always SEE a unit (including one wedged in Safe
  Mode) instead of scraping a stale `logs/{session}.log`. Shared `halPostHeartbeat()` in
  `airbridge_s3.h`. Emulator: `EMU_SAFE_MODE=1` runs the survival plane only;
  `scripts/cmd_channel_e2e.sh` Scenario 3 proves safe-mode reachability → heartbeat `mode=safe`
  → remote `format_sd` clears the firewall → `mode=healthy` (7/7).

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
├── src/main.cpp                 # Firmware entry: USB MSC, FreeRTOS task wiring, ESP-IDF glue (~4800 lines)
│
├── include/                     # ── Shared, hardware-independent logic (firmware + emulator + tests) ──
│   ├── DSU protocol & data
│   │   ├── airbridge_proto.h    #   DSU cookie builder, CRC-16, filename parser, chunked decode
│   │   ├── airbridge_harvest.h  #   Recursive dir walk, file move to /harvested/NNNN/, skip list;
│   │   │                        #   opt-in gzip of .eaofh into the queue (compress arg, -DAIRBRIDGE_COMPRESS)
│   │   └── airbridge_compress.h  #   Streaming gzip (zlib) for flight logs — ~3x fewer upload bytes
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
├── e2e_unified.sh               # Unified E2E suite (~25 tests; consolidated lifecycle + manifest +
│                                #   power-cut tests; state-triggered waits). --target emulator | device.
│                                #   Device runs the SAME consolidated tests via persistent CDC + a
│                                #   serial-log tap (/tmp/dev_e2e.log) + host_dsu.py (see docs/testing.md).
├── flight_cycle_test.sh         # Emulator soak test: NEW-aircraft backlog drain over repeated flight
│                                #   cycles, measuring cycles-to-catch-up (S3 manifest hwm == DSU latest) +
│                                #   backlog area-under-curve. Backlog lives in the DSU: a synthetic
│                                #   "internal memory" .eaofh (EMU_DSU_INTERNAL); the emulator's DSU thread
│                                #   streams un-sent flights over USB into flightHistory/ RATE-LIMITED at
│                                #   ~500 KB/s (EMU_DSU_KBPS) starting EMU_DSU_DELAY_MS (90s USB-present) after
│                                #   boot, one atomic file per flight, cursor/cookie-resumed across cycles —
│                                #   the device starts EMPTY. Each cycle's flight is recorded after the cycle
│                                #   ⇒ transferred on the NEXT power-on. Cellular scripted via EMU_CELL_FILE
│                                #   (emu/main.cpp applyCellState + sim_modem setFlaky/flakyDropsFrame: PPP_IP-only
│                                #   loss). Params: --backlog-mb/--cycles/--cruise-prob/--flaky/--uplink-kbps/
│                                #   --usb-kbps/--cruise-kb/--ground-kb/--seed/--fast.
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

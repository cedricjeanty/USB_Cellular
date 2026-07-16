# Field & Bench Operations

Device configuration and diagnostics. There is no interactive CLI — CDC is
log-only; everything goes through files on the SD card (or the same commands
over the cellular C2 channel for remote units).

## Serial / CDC

CDC is **log-only** (no RX callback, no interactive CLI). All configuration is via SD
magic files or `airbridge.cmd`. The interactive CLI was removed from `main.cpp`; its
command-parsing logic survives (extracted + unit-tested) in `airbridge_cli.h` but is not
wired up. STATUS is logged automatically every 60s via `airbridge_log()`.

## Dual-partition SD layout

In **dual-partition mode** (16GB+ cards), the SD card has two partitions:
- **P1** (8GB, USB-visible via MSC): DSU data — `flightHistory/`, `metrics/`, `dsuCookie.easdf`
- **P2** (rest, firmware-internal): logs, upload queue, magic files processed at boot

## Legacy SD magic files

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

## Unified command file (`airbridge.cmd`) — preferred

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
| `represent [on\|off]` | Re-present the USB drive (drop + re-raise D+ for ~3s) after every harvest that moved files, so the DSU re-enumerates, re-reads the advanced cookie, and may dump the NEXT flight in the same power session (post-landing taxi-in upload of the just-flown flight). Default ON; self-terminating — a harvest that moves nothing doesn't re-present. MSC-only builds only (CDC builds skip: re-enumeration would cut the bench serial tap) | runtime-safe (next harvest) |
| `flash` | **OTA-independent remote reflash.** Downloads the staged `firmware/latest.bin` (same as OTA — bump `latest.json` version to force it) into P2 `firmware.bin`, then reboots into the boot-time SD-flash path, which writes the sibling of the *running* partition. Survives a wedged `ota_data` / dead OTA slot that breaks the normal OTA path — so a unit reachable over cellular can always be reflashed without USB/BOOT. Acks `"flash":true` | runtime-safe |

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

## Antenna Signal Survey

For comparing antennas, the `survey` directive puts the unit in a **dedicated measurement
mode**: the modem task registers on the network and then **loops `modemSurveySample()`**
(`AT+CSQ` + `AT+CESQ` + `AT+CPSI` + `AT+COPS?`) every 2 s, logging
`SURVEY N: carrier=… band=… RSSI=… RSRP=… RSRQ=… SINR=…`. It **never dials PPP**, so the AT
polling never collides with an upload (mixing the two is what broke uploads before — once PPP
is up, escaping with `+++` to run AT commands stalls the data session). Survey mode and normal
upload operation are mutually exclusive (see `docs/decisions/0004`).

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

# Resilience: Watchdog, SD Fault Recovery, Safe Mode, OTA Resume

Read this before touching any failure or recovery path. The governing principle:
a deployed unit has no physical access, so every fault must degrade to
"reachable over cellular, awaiting orders" — never a brick. Decision records:
`docs/decisions/0003` (SD forward recovery), `docs/decisions/0005` (plane split).

## No-Progress Watchdog

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
is unacceptable).

## SD-Card Fault Robustness

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
reformat path, made visible by layers 1–2.

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

## Safe Mode / Crash-Loop Firewall + Heartbeat

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

## OTA Download Resume

`otaDownloadAndFlash()` keeps the `esp_ota` handle across up to 4 attempts and **resumes** a
dropped download with an HTTP `Range: bytes=<received>-` header (`buildRangeGetRequest()` in
`airbridge_http.h`, unit-tested) instead of re-fetching the whole image — so a transient link
hiccup mid-download no longer abandons the firmware (the cause of the unit staying on old fw).
`esp_ota_write` is sequential, so continuing to write the next bytes resumes cleanly; accepts
`200` (full) on attempt 0 and `206` (partial) on resume, and restarts cleanly if a server
ignores `Range` and resends from 0.

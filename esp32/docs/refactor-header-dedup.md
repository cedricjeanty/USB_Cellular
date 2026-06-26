# Firmware ⇄ Emulator De-duplication Plan

**Goal:** make the emulator emulate the real device as faithfully as possible by
**reusing the device firmware's own logic**, rather than maintaining a parallel
reimplementation in `emu/main.cpp` that has to be hand-synced (the tell-tale
`// mirrors main_loop_task` / `// like uploadTask does` / `// Mirrors the
firmware's sd_health_check` comments). Every hand-synced copy is a place the
emulator can silently drift from the device.

## Principle: push the platform boundary *down*

```
┌──────────────────────────────────────────────────────────────┐
│  Orchestration / duty cycle  (tasks, sequencing, state machine)│  ← today: DUPLICATED
├──────────────────────────────────────────────────────────────┤
│  Domain logic  (proto, harvest, s3, modem AT, sdRecoveryAction)│  ← already SHARED (airbridge_*.h)
├──────────────────────────────────────────────────────────────┤
│  HAL  (nvs / uart / network / filesys / display / clock)       │  ← shared interface, per-platform impls
├──────────────────────────────────────────────────────────────┤
│  Hardware glue  (TinyUSB, sdmmc regs, esp_ota, esp_restart,     │  ← stays per-platform
│                  FreeRTOS xTaskCreate)                          │
└──────────────────────────────────────────────────────────────┘
```

Leaf domain logic is already shared via `airbridge_*.h`. The gap is the
**middle layer** — the orchestration. The plan moves it into shared, HAL-backed
modules so the emulator threads and the firmware tasks call the *same* functions,
leaving only true hardware behind thin HAL hooks.

## Phases (each a reviewable gate; device stays green throughout)

### Phase 1 — Shared SD service  *(in progress)*
Unify the SD/FatFs orchestration so the firmware and the emulator/tests run one
codebase.

- **1a — Dual-partition format sequence — DONE.** `airbridge_sd.h::sdFormatDual`
  holds the `f_fdisk` + MBR-fixup + `f_mkfs(P1/P2, FM_SFD)` sequence (the
  riskiest SD code — a bug bricks the card). Platform differences (block device,
  diskio registration, sector-0 IO) are injected via `SdDiskOps`. The firmware's
  `FmtTask` and the block test's `run_full_format` now both call it, so
  `test_native_sd_block` exercises the exact bytes the device writes.
- **1b — Health state machine + emulator on the real layout — DONE.** The
  emulator's `FatFsFilesys` now formats via `sdFormatDual` and mounts the real
  **dual-partition** layout (P1→`2:`, P2→`1:`) instead of a single-volume FAT, so
  it boots the device's actual partition scheme. The health *state machine*
  (consecutive-failure counter → escalation) is extracted to
  `airbridge_runtime.h::sdHealthUpdate(SdHealthState&, probeOk, …)`; the firmware's
  `sd_health_check` and the emulator's health loop now run that ONE function —
  only the platform-specific degrade/reformat *actions* (firmware: mark unmounted
  + reboot-to-reformat; emulator: set OLED flag + reformat-in-place) stay in glue.
  (Still per-platform: the *mount* call itself — the firmware uses `esp_vfs_fat`
  for its POSIX layer, the emulator raw `f_mount` — a candidate for a later pass.)
- **1c — MBR / partition detection.** Fold `sd_init`'s case analysis (valid MBR /
  add-P2 migration / no-MBR → reformat) into shared byte-level helpers
  (`test_native_sd_format` already covers the MBR math).

### Phase 2 — Shared duty-cycle / orchestration  *(partially done; upload routing HARDWARE-GATED)*
Extract the task *bodies* into HAL-backed step functions that both the firmware's
FreeRTOS tasks and the emulator's threads invoke.

- **Modem reconnect escalation — DONE.** `modemReconnectPlan(failures)` (shared,
  unit-tested) replaces the inline soft/CFUN/backoff decision in `modemTask`. The
  AT sequence was already shared (`modemReconnect`), and the PTY + `sim_modem` +
  `pppd` harness exercises it through real code, so this is validated.

- **Upload routing — DEFERRED, hardware-gated.** Routing the firmware's
  `s3UploadFileEx` (direct esp_tls) through the shared `airbridge_s3.h`
  (`halS3UploadFile`) is feasible (the firmware already has `Esp32Network`/
  `Esp32Filesys`, and `halS3UploadFile` has the `keyOverride` parity) BUT carries
  a concrete, emulator-invisible regression risk:

  The firmware holds `g_tlsActive = true` for the **entire multipart session**
  (main.cpp:1920) to suppress the modem watchdog's `+++` escape *between* part
  PUTs. `airbridge_s3.h` has no concept of `g_tlsActive` — `Esp32Network` toggles
  it per-TLS-connect, so a shared-path multipart would leave gaps between parts
  where the watchdog could fire `+++` and stall the PPP data session (the bug
  class CLAUDE.md notes "broke uploads before"). The PTY/pppd emulator cannot
  reproduce this — there is no real modem watchdog racing `+++` — so native +
  emulator green would be a false guarantee.

  To land safely: add session-scoped `+++` suppression to the shared path (e.g. a
  HAL "upload session begin/end" hook), keep `s3UploadFileEx` behind a build flag
  for instant A/B revert, and gate merge on a hardware multipart-upload test on a
  marginal link. Until that hardware loop exists, the firmware keeps its proven
  esp_tls upload.

The remaining `// mirrors …` orchestration (the upload/harvest/log sequencing in
`main_loop`) rides on the upload routing, so it is gated the same way.

### Phase 3 — Thin task/timer shim
A minimal abstraction so the emulator spawns host threads running the *same*
task functions the device runs under FreeRTOS, and `esp_restart` / USB-present /
OTA-flash become 3–4 HAL hooks the emulator stubs.

## What stays per-platform (intentionally)
TinyUSB MSC callbacks, the `sdmmc`/`sdspi` register init, `esp_ota` flash write,
`esp_restart`, and the FreeRTOS task-creation primitives. These are wrapped
behind small HAL hooks, not shared.

## The host-directory backend
Once the SD service is shared (Phase 1b), the emulator runs the real FatFs/SD
code by default (`EMU_SD_BLOCK`). The host directories then survive only as a
**seed/inspection adapter** (import-on-drop + dump-on-demand to/from the FAT
image) so the interactive drop-files workflow and the e2e suite keep working on
top of the real code — not as a second filesystem. See `include/hal/fatfs_filesys.h`
and `scripts/test_emu_sd_block.sh`.

## Verification per phase
- Native unit tests green (`pio test -e native`).
- Firmware builds (`pio run -e esp32s3 -e esp32s3-e2e`).
- Emulator builds + default mode unchanged; `scripts/test_emu_sd_block.sh` green.
- For SD changes: the on-card byte layout is pinned by `test_native_sd_block` /
  `test_native_sd_format`, so a shared extraction that keeps those green is
  behavior-preserving (the format path can't be fully exercised on real hardware
  in CI).

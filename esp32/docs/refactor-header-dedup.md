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
- **1b — Mount + health probe + recovery.** Extract `sd_init`'s mount path and
  `sd_health_check`'s probe into shared helpers over `SdDiskOps` + the existing
  `sdRecoveryAction`. The firmware backs them with sdmmc; the emulator with
  `FakeSd`. Goal: `sd_health_check` becomes one shared function both platforms
  call, and the emulator's `FatFsFilesys` moves from a single-volume FAT to the
  real dual-partition layout via `sdFormatDual`.
- **1c — MBR / partition detection.** Fold `sd_init`'s case analysis (valid MBR /
  add-P2 migration / no-MBR → reformat) into shared byte-level helpers
  (`test_native_sd_format` already covers the MBR math).

### Phase 2 — Shared duty-cycle / orchestration
Extract the task *bodies* into HAL-backed step functions — `uploadStep()`,
`harvestStep()`, `modemPump()`, `logFlushStep()` — that both the firmware's
FreeRTOS tasks and the emulator's threads invoke. The `// mirrors …` comments in
`emu/main.cpp` disappear because there is nothing left to mirror.

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

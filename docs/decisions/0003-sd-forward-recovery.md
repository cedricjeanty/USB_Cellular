# 0003 — SD fault recovery is forward (degrade → reformat), never a reboot loop

- **Status**: accepted
- **Date**: 2026-06-20 (backfilled 2026-07-07)

## Context

A corrupt or failing SD card must never silently dark a deployed unit — there is
no tech on site. A reboot cannot fix a corrupt FAT, so any retry/reboot-based
"recovery" just loops forever while the unit stays dark. The data on the card is
recoverable from upstream: the DSU cookie protocol makes the aircraft re-send
whatever the device lost.

## Decision

Recovery escalates forward through `sdRecoveryAction()` (unit-tested, in
`airbridge_runtime.h`): **DEGRADE** first (mark unmounted, surface on OLED, keep
logging over cellular via the SD-independent RAM ring buffer, retry the remount)
— a transient flake recovers here with no data loss; then **REFORMAT** if the
card stays dead — destructive, data loss accepted, because the DSU re-sends the
lost queue via the cookie. There is deliberately **no boot watchdog** for SD
faults. The reformat event is recorded in NVS so it is reported after reboot.

Rejected: reboot-on-mount-failure (can't fix corruption, loops); fsck-style FAT
repair on device (complexity and flash budget, still not guaranteed).

## Consequences

- Worst-case SD failure costs the current upload queue, not the unit.
- Log egress and heartbeat must stay SD-independent (a constraint on any new
  logging/telemetry code).
- Details and test coverage (FakeSd corruption suites, EMU_SD_BLOCK):
  docs/resilience.md.

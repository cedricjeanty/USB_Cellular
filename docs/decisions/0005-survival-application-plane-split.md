# 0005 — Survival plane / application plane split (Safe Mode firewall)

- **Status**: accepted
- **Date**: 2026-07-04 (backfilled 2026-07-07)

## Context

Deployed units have no physical access. A data fault (corrupt SD, bad config, an
app-init panic) that crash-loops the firmware used to produce an unreachable
brick — the crash-loop counter reset every boot, so the `boots>5` guard never
fired and there was effectively no crash-loop protection. A 9-hour frozen unit
on an aircraft (see docs/resilience.md) made the requirement concrete: any
fault must degrade to "connected and awaiting orders."

## Decision

Split the firmware into a **survival plane** (cellular + remote command channel
+ heartbeat/log egress — all SD-independent) and an **application plane** (SD,
USB MSC, harvest, upload). `decideBootMode()` (pure, unit-tested) sends the
device to Safe Mode after 3 consecutive boots that never reached stable
(non-power-on resets only — a human power-cycle always gets one normal retry).
Safe Mode boots ONLY the survival plane and stays there until a deliberate
remote fix (remote `format_sd`, OTA self-heal, or pending-OTA rollback). A 60s
heartbeat POSTs in both modes so an operator can always see the unit.

Rejected: OTA rollback alone (covers bad firmware but not data faults);
unconditional full boot with more retries (a corrupt SD crash-loops forever);
watchdog-only protection (reboots don't clear the fault).

## Consequences

- A crash loop is now an observable, remotely recoverable state, never a brick.
- Everything in the survival plane must remain SD-independent — a standing
  constraint on modem-task, C2, heartbeat, and log-egress code.
- The crash-loop counter (`dbg/boots`) resets ONLY at the ~60s "mark healthy"
  point; code that resets it elsewhere reintroduces the original bug.
- Full mechanics: docs/resilience.md.

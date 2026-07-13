# 0006 — Reconnect must restore full baud; factory reset gated on signal evidence

- **Status**: accepted
- **Date**: 2026-07-12

## Context

The device must re-establish cellular and upload flight logs during the
post-landing taxi-in — a window of minutes. A flight spends 30+ minutes out of
coverage, so by touchdown the reconnect ladder has accumulated dozens of
failures. Two problems followed from that:

1. `modemReconnect(resetRadio=true)` (CFUN=0/1, every 5th attempt) dropped both
   sides to 115200 and deferred the 3 Mbaud + HW-flow-control upgrade "to the
   next boot". Any flight long enough to trigger one radio reset therefore
   landed with a ~10 KB/s session instead of 73-167 KB/s — gutting the taxi-in
   upload. Similarly, a factory reset rebooted the module to 115200/echo-on with
   no host-side recovery at all.
2. The factory-reset tier (every 12th failure) was designed for a unit stranded
   by a stale band-lock (`survey band=N`) or manual COPS — but it fired just as
   readily when the counter climbed from ordinary airborne no-coverage, causing
   repeated needless module reboots mid-flight and a ~1-min dead window if one
   coincided with coverage returning at touchdown.

## Decision

We will re-run the baud/flow-control upgrade ladder (extracted as the shared
`modemUpgradeBaud()`) after **every** event that resets the modem UART: inside
the radio-reset branch of `modemReconnect()`, and via `modemFactoryRecover()`
(AT resync + ATE0 + upgrade) after both factory-reset paths. "Wait for the next
boot" was rejected: the post-reset session is exactly the one that must upload
fast.

We will gate the factory-reset cadence on signal evidence
(`modemReconnectPlan(failures, signalSeen)`): every 12th failure when a cell
has been visible (CSQ != 99, or registration achieved) since the last good
session yet service still fails — the genuinely-stranded case; every 36th when
the radio has seen nothing at all — most likely airborne. `modemWaitForSignal`
now reads CSQ on every poll (valid while unregistered) to supply that evidence.
Rejected alternatives: removing the no-signal tier entirely (loses the
never-stranded guarantee — a band-locked radio also sees nothing, so the two
cases are indistinguishable); CEREG stat=3 "denied" as the trigger (too narrow —
a dead-carrier PLMN registers fine).

## Consequences

- A landing reconnect comes back at full speed regardless of how many resets the
  flight accumulated; the CFUN-reset tier is now cheap enough that its every-5th
  cadence needs no gating.
- Long no-coverage stretches reboot the module ~3× less often (every 36 vs 12);
  a truly band-locked unit takes up to ~90 min (vs ~30) to self-recover — an
  acceptable trade since band locks are only ever self-inflicted via `survey`.
- SimModem now models CRESET/AT&F (baud→115200, echo on, PDP cleared), and the
  native suites assert baud restoration end-to-end. Revisit if a real SIM7600
  is observed failing the post-CFUN IPR upgrade (would strand the session at
  115200 — the ladder falls back but only re-tries at the next reset).

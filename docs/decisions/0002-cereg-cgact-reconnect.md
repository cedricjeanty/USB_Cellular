# 0002 — Modem reconnect checks CEREG (never CREG) and always runs CGACT=0,1; CFUN is last resort

- **Status**: accepted
- **Date**: 2026-05-27 (fw 20260527140000/20260527160000)

## Context

The carrier (T-Mobile via Hologram) terminates PPP sessions every ~4-6 hours —
normal behavior that the device must recover from unattended. Early firmware
treated every drop with `AT+CREG?` checks and CFUN=0/1 radio resets, producing
minutes-long outages and spurious resets.

Two facts drive the design: (1) T-Mobile *denies voice* registration (CREG
returns 3 = denied) while *granting LTE data* registration (CEREG returns 5 =
roaming), so a CREG check always times out on this SIM; (2) after a
carrier-terminated session the radio is still registered — CFUN=0/1 forces a
real deregistration and a 30-60s re-attach, making recovery slower.

## Decision

Soft reconnect is the default path: `+++` guard → `ATH` → `AT+CGACT=0,1` → 5s →
`AT+CEREG?`/`AT+CGREG?` → `AT+CGDCONT=1,"IP","hologram"` → `ATD*99#`. The `+++`
guard is mandatory (a stalled IPCP leaves the modem in data mode where ATH/CEREG
are ignored as PPP bytes). `AT+CGACT=0,1` is mandatory before every redial (a
stale PDP context gives CONNECT with no IP). CFUN=0/1 is reserved for a
genuinely unresponsive modem, escalated by the backoff schedule (failures 4 and
9, then progressive delays).

Rejected: CREG-based checks (always time out on this SIM); CFUN-first recovery
(slower and unnecessary after carrier termination).

## Consequences

- Reconnect after carrier termination completes in seconds, not minutes.
- Any `AT+CREG` appearing in modem code is a bug — flag it in review.
- Full sequences, backoff schedule, and pppStale threshold: docs/modem.md.

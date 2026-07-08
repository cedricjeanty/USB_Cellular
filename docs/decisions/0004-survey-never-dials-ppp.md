# 0004 — Antenna survey is a dedicated mode that never dials PPP

- **Status**: accepted
- **Date**: 2026-06-28 (backfilled 2026-07-07)

## Context

Comparing antennas needs continuous signal metrics (RSSI/RSRP/RSRQ/SINR/band).
Polling AT commands during a live PPP session requires `+++` escapes, which
stall the data session — mixing signal polling with uploads is what broke
uploads before. Additionally, the Hologram multi-carrier SIM freely reselects
band/cell, swinging RSRP 10+ dB and swamping real antenna differences (one
antenna looked "13 dB better" purely from a Band 5 vs Band 4 camp).

## Decision

The `survey` directive puts the unit in a measurement-only mode: register on the
network, loop `modemSurveySample()` every 2s, and **never dial PPP**. Survey
mode and upload operation are mutually exclusive. For valid comparisons,
`survey band=N` locks the LTE band (`AT+CNMP=38` + `AT+CNBP`); `survey
band=auto` MUST be run before a unit returns to service because CNMP/CNBP
persist in the modem's own NVS across reboot and reflash.

Rejected: polling signal during uploads via `+++` (stalls the data session);
one-shot signal reads per boot (too sparse to compare antennas).

## Consequences

- Antenna work is safe and repeatable (swap antennas without reboots), but a
  surveyed unit is out of service until the directive is removed.
- The band-lock persistence is a field hazard: forgetting `band=auto` ships a
  unit locked to one band. Checklists/procedures: docs/operations.md.

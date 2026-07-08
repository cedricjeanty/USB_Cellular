# 0001 — Shared headers + emulator-first development

- **Status**: accepted
- **Date**: 2026-01-15 (backfilled 2026-07-07)

## Context

The firmware started as a single ~5000-line `main.cpp` tied to ESP-IDF. Bugs
were only reproducible on hardware, iteration required flashing a device, and
logic fixes risked diverging between the firmware and any test double. Deployed
units are on aircraft — a regression that only shows up in the field is very
expensive.

## Decision

All hardware-independent logic lives in `esp32/include/airbridge_*.h` headers
that compile into THREE targets: the firmware (`src/main.cpp`, glued to ESP-IDF
drivers), the SDL2 emulator (`emu/main.cpp`, glued to FakeSd/FileNvs/SimModem),
and the native Unity test suites — all through the HAL interfaces in
`esp32/include/hal/`. The workflow rule follows from the structure: replicate a
bug in the emulator first, fix it in the shared header, add a native test; never
duplicate logic in `main.cpp`.

Rejected: a separate emulator codebase (drifts from the firmware, tests prove
nothing about shipped code); hardware-only testing (too slow, can't inject
faults like SD corruption or PPP drops safely).

## Consequences

- Every fix/feature lands in a header with a matching `test/test_native_*` suite.
- The emulator must keep simulating what the firmware experiences (modem timing,
  DSU behavior, SD block devices) — fidelity work is an ongoing cost that has
  repeatedly paid for itself (power-cut, SD-corruption, and C2 E2E suites).
- `main.cpp` shrinks over time (see docs/refactor-header-dedup.md for the
  remaining firmware-only paths).

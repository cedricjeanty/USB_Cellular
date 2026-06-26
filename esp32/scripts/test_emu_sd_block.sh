#!/usr/bin/env bash
# Headless emulator test for the FakeSd-backed SD (EMU_SD_BLOCK=1): drives the
# corrupt-FAT → degrade → reformat → recover lifecycle through REAL FatFs on the
# in-memory FakeSd block device and asserts the expected log transitions.
#
# This is the end-to-end counterpart to the unit tests:
#   - test_native_fatfsfs   — the FatFsFilesys backend in isolation
#   - test_native_sd_block  — block-level FatFs corruption/reformat
#   - test_native_runtime   — sdRecoveryAction escalation logic
# Here the same pieces run inside the actual emulator binary.
#
# Usage: scripts/test_emu_sd_block.sh   (run from the esp32/ dir; builds if needed)
set -euo pipefail

cd "$(dirname "$0")/.."
BIN=".pio/build/emulator/program"
[ -x "$BIN" ] || { echo "Building emulator..."; pio run -e emulator >/dev/null; }
BIN="$(pwd)/$BIN"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"
mkdir -p emu_sdcard/flightHistory
printf 'FLIGHTDATA12345' > emu_sdcard/flightHistory/FLT00099_test.eaofh

echo "Running emulator (block mode, auto-corrupt @3s into the run, ~22s)..."
LOG="$WORK/emu_out.log"
EMU_SD_BLOCK=1 EMU_SD_CORRUPT_AFTER_MS=3000 timeout 22 "$BIN" TESTDEV01 > "$LOG" 2>&1 || true

echo "--- SD lifecycle ---"
grep -iE "block mode|corruption|degraded|reformat|recovered|imported" "$LOG" || true
echo "--------------------"

fail=0
check() { if grep -qiE "$1" "$LOG"; then echo "  PASS: $2"; else echo "  FAIL: $2"; fail=1; fi; }
check "FakeSd block mode .*imported 1"            "block mode active + host file imported into the FAT image"
check "TEST corruption injected"                  "FAT corruption injected"
check "runtime failure .* degraded"               "degrade tier reached (SD ERROR surfaced)"
check "unrecoverable .* reformatting"             "escalated to reformat"
check "reformatted .* recovered"                  "card recovered after reformat"

if [ "$fail" -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED — corrupt → degrade → reformat → recover verified through FatFs on FakeSd"

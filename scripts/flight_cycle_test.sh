#!/bin/bash
# AirBridge flight-cycle soak test (EMULATOR ONLY).
#
# Simulates repeated flight cycles over a scripted cellular profile and measures
# how many cycles it takes the device to upload a full DSU backlog and CATCH UP
# to the latest flight (S3 manifest high_water_mark == latest flight number).
#
# Cycle profiles (durations compressed to seconds — the SHAPE is what matters):
#   Cruise: power on → full (climb-out upload) → weak → off (cruise: flight
#           recorded, NO cell) → flaky (approach: lossy uploads + retries) →
#           full (taxi: clean catch-up) → power off.  DSU gains a ~CRUISE_KB flight.
#   Ground: power on → full short window → power off.  DSU gains a ~GROUND_KB flight.
#
# Each cycle the DSU gains ONE new single-flight file; the device drains the
# backlog oldest-first across cycles. A clean link (FLAKY_PCT=0) catches up faster
# than a flaky one — the metric is sensitive to the profile.
#
# Cellular is scripted via EMU_CELL_FILE (full|weak|off|flaky), which the emulator
# polls (see emu/main.cpp applyCellState + sim_modem setFlaky). HARDWARE is out of
# scope here — you can't script tower loss at the bench.
#
# Usage:
#   scripts/flight_cycle_test.sh [--cycles N] [--backlog M] [--ratio C:G]
#                                [--flaky PCT] [--cruise-kb KB] [--ground-kb KB]
# Sizes default to emulator-scaled values (override --cruise-kb 10240 for real 10MB).
set -u

TARGET="emulator"                 # this test is emulator-only
source "$(dirname "$0")/e2e_lib.sh"

# ── Params (env-overridable; CLI flags below) ────────────────────────────────
CYCLES="${CYCLES:-30}"
BACKLOG="${BACKLOG:-6}"
RATIO="${RATIO:-2:1}"             # cruise:ground mix
FLAKY_PCT="${FLAKY_PCT:-30}"
CRUISE_KB="${CRUISE_KB:-2048}"    # emulator-scaled "cruise" flight (~10MB real)
GROUND_KB="${GROUND_KB:-200}"     # short ground-run flight
# compressed phase durations (emulator seconds)
T_PRETAKEOFF="${T_PRETAKEOFF:-8}"
T_TAKEOFF="${T_TAKEOFF:-3}"
T_CRUISE="${T_CRUISE:-6}"
T_APPROACH="${T_APPROACH:-10}"
T_TAXI="${T_TAXI:-10}"
T_GROUND="${T_GROUND:-10}"

while [ $# -gt 0 ]; do
    case "$1" in
        --cycles)    CYCLES="$2"; shift 2 ;;
        --backlog)   BACKLOG="$2"; shift 2 ;;
        --ratio)     RATIO="$2"; shift 2 ;;
        --flaky)     FLAKY_PCT="$2"; shift 2 ;;
        --cruise-kb) CRUISE_KB="$2"; shift 2 ;;
        --ground-kb) GROUND_KB="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

EMU_CELL_FILE="$FW_DIR/emu_cell.ctl"
export EMU_CELL_FILE
export EMU_CELL_SEED="${EMU_CELL_SEED:-1}"   # reproducible flaky behavior
PASS=0; FAIL=0; SKIP=0

# ── Helpers ──────────────────────────────────────────────────────────────────

# Atomically set the scripted cellular state (temp + rename so the emulator never
# reads a half-written word).
cell_set() {
    local word="$1" arg="${2:-}"
    local tmp="${EMU_CELL_FILE}.tmp"
    if [ -n "$arg" ]; then echo "$word $arg" > "$tmp"; else echo "$word" > "$tmp"; fi
    mv -f "$tmp" "$EMU_CELL_FILE"
    log "    cell → $word${arg:+ $arg}"
}

# Write ONE discrete flight log to the DSU: first record == last record == flight,
# so each cycle adds a distinct flight (vs e2e_lib's write_dsu_file which spans
# 1..N). Reuses the sourced append_eaofh_trailer. Emulator FakeSD path.
write_flight() {
    local flight="$1" size_kb="$2"
    local f5; f5=$(printf '%05d' "$flight")
    mkdir -p "$SD_EMU/flightHistory"
    local fpath="$SD_EMU/flightHistory/${SERIAL}_${f5}_$(date +%Y%m%d).eaofh"
    append_eaofh_trailer "$fpath" "$SERIAL" "$f5"     # first record = this flight
    printf '\xEA' >> "$fpath"                          # framing byte after first record
    dd if=/dev/urandom of="$fpath" bs=1K count="$size_kb" oflag=append conv=notrunc 2>/dev/null
    append_eaofh_trailer "$fpath" "$SERIAL" "$f5"     # last record = this flight
    log "    DSU += flight $flight (${size_kb}KB)"
}

seed_backlog() {
    local m="$1" f
    for f in $(seq 1 "$m"); do
        if [ $((f % 3)) -eq 0 ]; then write_flight "$f" "$GROUND_KB"; else write_flight "$f" "$CRUISE_KB"; fi
    done
    log "Seeded backlog: flights 1..$m (S3 manifest hwm=0)"
}

# One cruise cycle: scripted full→weak→off(+new flight)→flaky→full→shutdown.
run_cruise_cycle() {
    local flight="$1"
    cell_set full           # boot lands in "full"; set it before bring-up
    start_device
    cell_set full;  sleep "$T_PRETAKEOFF"                      # climb-out upload window
    cell_set weak;  sleep "$T_TAKEOFF"                          # signal fading
    cell_set off;   write_flight "$flight" "$CRUISE_KB"; sleep "$T_CRUISE"   # cruise: flight recorded, no cell
    cell_set flaky "$FLAKY_PCT"; sleep "$T_APPROACH"           # approach: lossy uploads
    cell_set full;  sleep "$T_TAXI"                            # taxi: clean catch-up
    stop_device
}

# One quick ground cycle: short powered window with good cell, small flight.
run_ground_cycle() {
    local flight="$1"
    cell_set full
    start_device
    write_flight "$flight" "$GROUND_KB"
    sleep "$T_GROUND"
    stop_device
}

# Build the cruise/ground step sequence from --ratio C:G, length = CYCLES.
build_mix() {
    local c="${RATIO%%:*}" g="${RATIO##*:}" seq="" i
    while [ "$(echo "$seq" | wc -w)" -lt "$CYCLES" ]; do
        for i in $(seq 1 "$c"); do seq="$seq C"; done
        for i in $(seq 1 "$g"); do seq="$seq G"; done
    done
    echo "$seq" | tr ' ' '\n' | grep . | head -n "$CYCLES" | tr '\n' ' '
}

# ── Run ──────────────────────────────────────────────────────────────────────
log "═══════════════════════════════════════════════════════════════"
log "  Flight-cycle soak test (emulator)"
log "  backlog=$BACKLOG cycles=$CYCLES ratio=$RATIO flaky=${FLAKY_PCT}%"
log "  cruise=${CRUISE_KB}KB ground=${GROUND_KB}KB seed=$EMU_CELL_SEED"
log "═══════════════════════════════════════════════════════════════"

# Fresh state: wipe local SD/NVS/cookie + S3 manifest (start at hwm=0).
stop_device 2>/dev/null || true
rm -rf "$SD_EMU"/* "$SD_INT"/* "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_modem.dat" "$EMU_CELL_FILE" 2>/dev/null || true
cleanup_s3; cleanup_aircraft_s3 "$SERIAL"
cd "$FW_DIR" && ~/.local/bin/pio run -e emulator 2>&1 | tail -1

seed_backlog "$BACKLOG"
MIX=$(build_mix)
log "Mix: $MIX"

latest="$BACKLOG"; catchup=-1; cycle=0
for step in $MIX; do
    cycle=$((cycle + 1)); latest=$((latest + 1))
    log ""
    log "── cycle $cycle ($([ "$step" = C ] && echo cruise || echo ground)) — DSU latest will be $latest ──"
    if [ "$step" = C ]; then run_cruise_cycle "$latest"; else run_ground_cycle "$latest"; fi
    hwm=$(get_manifest_hwm "$SERIAL")
    log "  RESULT cycle=$cycle step=$step latest=$latest s3_hwm=$hwm"
    if [ "${hwm:-0}" -ge "$latest" ] && [ "$catchup" -lt 0 ]; then
        catchup=$cycle
        log "  ★ CAUGHT UP at cycle $cycle (hwm=$hwm == latest=$latest)"
        break
    fi
done

stop_device 2>/dev/null || true
final_hwm=$(get_manifest_hwm "$SERIAL")
log ""
log "═══════════════════════════════════════════════════════════════"
if [ "$catchup" -gt 0 ]; then
    pass "Caught up to latest flight ($latest) in $catchup cycles"
else
    fail "Did NOT catch up in $cycle cycles (latest=$latest final_hwm=$final_hwm)"
fi
log "  Summary: backlog=$BACKLOG, ran $cycle cycles, catchup_cycle=$catchup, final_hwm=$final_hwm"
log "═══════════════════════════════════════════════════════════════"
cleanup_aircraft_s3 "$SERIAL"
exit "$FAIL"

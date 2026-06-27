#!/bin/bash
# AirBridge flight-cycle soak / harvest-optimization benchmark (EMULATOR ONLY).
#
# Simulates repeated flight cycles over a scripted cellular profile and measures
# how effectively the device drains an initial DSU backlog (~hundreds of MB to
# ~1 GB) toward catch-up. Designed for A/B optimization of the harvest/upload:
#
#   • SEEDED SCENARIOS — --seed makes everything that varies (cycle type,
#     per-cycle phase-duration jitter, per-flight sizes, the flaky drop pattern)
#     deterministic. Same seed ⇒ identical scenario ⇒ run-to-run comparable.
#     A fixed SEED SET (e.g. 1..N) is your benchmark suite; real-world variability
#     comes from the spread across seeds.
#   • PRIMARY METRIC — backlog AREA-UNDER-CURVE: Σ over cycles of the un-uploaded
#     backlog (reported in flight-cycles AND MB-cycles). Always defined, lower =
#     better harvest. Secondary: cycles-to-catch-up + final backlog. A CSV trace
#     is written for plotting/comparison.
#
# Cellular is scripted via EMU_CELL_FILE (full→weak→off→flaky→full); see
# emu/main.cpp applyCellState + sim_modem setFlaky. Real uploads to S3 over pppd.
#
# Usage:
#   scripts/flight_cycle_test.sh --backlog-mb 200 --seed 1            # real soak (hours)
#   scripts/flight_cycle_test.sh --fast --backlog-mb 6 --seed 1       # quick validation
set -u

TARGET="emulator"
source "$(dirname "$0")/e2e_lib.sh"

# ── Params ───────────────────────────────────────────────────────────────────
SEED="${SEED:-1}"
BACKLOG_MB="${BACKLOG_MB:-200}"      # initial un-uploaded backlog target (MB)
CYCLES="${CYCLES:-15}"
CRUISE_PROB="${CRUISE_PROB:-75}"     # % of cycles that are cruise (else quick-ground)
FLAKY_PCT="${FLAKY_PCT:-30}"
UPLINK_KBPS="${UPLINK_KBPS:-167}"    # emulator uplink (167 ≈ real PCB); EMU_UPLINK_KBPS
CRUISE_KB="${CRUISE_KB:-10240}"      # mean cruise flight size (~10MB)
GROUND_KB="${GROUND_KB:-300}"        # mean quick-ground flight size
# Real-world phase-duration bases (seconds), jittered ±20% per cycle by the seed.
T_PRETAKEOFF="${T_PRETAKEOFF:-180}"; T_TAKEOFF="${T_TAKEOFF:-45}"
T_CRUISE="${T_CRUISE:-300}";         T_APPROACH="${T_APPROACH:-240}"
T_TAXI="${T_TAXI:-180}";             T_GROUND="${T_GROUND:-240}"
# USB-presentation / DSU-connect delay: the aircraft DSU only writes the new
# flight to the device ~90s after power-on (the firmware holds D+ low until then).
# The backlog already on the device drains from boot; only the NEW flight per
# cycle is gated by this. For a short ground cycle this eats much of the window.
USB_PRESENT="${USB_PRESENT:-90}"
FAST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --seed)        SEED="$2"; shift 2 ;;
        --backlog-mb)  BACKLOG_MB="$2"; shift 2 ;;
        --cycles)      CYCLES="$2"; shift 2 ;;
        --cruise-prob) CRUISE_PROB="$2"; shift 2 ;;
        --flaky)       FLAKY_PCT="$2"; shift 2 ;;
        --uplink-kbps) UPLINK_KBPS="$2"; shift 2 ;;
        --cruise-kb)   CRUISE_KB="$2"; shift 2 ;;
        --ground-kb)   GROUND_KB="$2"; shift 2 ;;
        --fast)        FAST=1; shift ;;   # compressed durations + fast link, for validation
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done
if [ "$FAST" = 1 ]; then
    T_PRETAKEOFF=6; T_TAKEOFF=2; T_CRUISE=5; T_APPROACH=8; T_TAXI=8; T_GROUND=8
    UPLINK_KBPS="${UPLINK_KBPS_FAST:-4000}"
    USB_PRESENT="${USB_PRESENT_FAST:-4}"
fi

EMU_CELL_FILE="$FW_DIR/emu_cell.ctl"; export EMU_CELL_FILE
export EMU_CELL_SEED="$SEED"          # seed the emulator's flaky-drop RNG too
export EMU_UPLINK_KBPS="$UPLINK_KBPS"
CSV="/tmp/flight_cycle_seed${SEED}.csv"
PASS=0; FAIL=0; SKIP=0
RANDOM="$SEED"                         # deterministic bash RNG for the scenario

# ── Seeded RNG helper ────────────────────────────────────────────────────────
# Sets global JIT = base ± pct%, drawing from the PARENT shell's RANDOM (must NOT
# be called in a $( ) subshell, or RANDOM won't advance and draws repeat). This
# is what makes the whole scenario a deterministic function of --seed.
JIT=0
jit() { local base=$1 pct=$2; local d=$((base*pct/100)); [ "$d" -lt 1 ] && d=1; JIT=$((base - d + RANDOM % (2*d+1))); }

# Per-flight size store (KB), indexed by flight number, for MB-backlog accounting.
declare -A FSIZE

# ── Helpers ──────────────────────────────────────────────────────────────────
cell_set() {
    local word="$1" arg="${2:-}" tmp="${EMU_CELL_FILE}.tmp"
    if [ -n "$arg" ]; then echo "$word $arg" > "$tmp"; else echo "$word" > "$tmp"; fi
    mv -f "$tmp" "$EMU_CELL_FILE"; log "    cell → $word${arg:+ $arg}"
}

# One discrete single-flight log (first record == last record == flight).
write_flight() {
    local flight="$1" size_kb="$2"; local f5; f5=$(printf '%05d' "$flight")
    mkdir -p "$SD_EMU/flightHistory"
    local fpath="$SD_EMU/flightHistory/${SERIAL}_${f5}_$(date +%Y%m%d).eaofh"
    append_eaofh_trailer "$fpath" "$SERIAL" "$f5"
    printf '\xEA' >> "$fpath"
    dd if=/dev/urandom of="$fpath" bs=1K count="$size_kb" oflag=append conv=notrunc 2>/dev/null
    append_eaofh_trailer "$fpath" "$SERIAL" "$f5"
    FSIZE[$flight]=$size_kb
}

# MB of backlog above a given hwm (sum of un-uploaded flight sizes).
backlog_mb_above() {
    local hwm="$1" f kb=0
    for f in $(seq $((hwm+1)) "$LATEST"); do kb=$((kb + ${FSIZE[$f]:-0})); done
    echo $((kb/1024))
}

# Seed the initial backlog: discrete flights until ~BACKLOG_MB, mostly cruise-sized.
seed_backlog() {
    local total_kb=0 target_kb=$((BACKLOG_MB*1024)) f=0
    while [ "$total_kb" -lt "$target_kb" ]; do
        f=$((f+1))
        if [ $((RANDOM % 100)) -lt "$CRUISE_PROB" ]; then jit "$CRUISE_KB" 20; else jit "$GROUND_KB" 30; fi
        write_flight "$f" "$JIT"; total_kb=$((total_kb+JIT))
    done
    BACKLOG_FLIGHTS=$f; LATEST=$f
    log "Seeded backlog: $BACKLOG_FLIGHTS flights ≈ $((total_kb/1024))MB (target ${BACKLOG_MB}MB), S3 hwm=0"
}

# Each cycle the DSU reconnects ~USB_PRESENT(90s) in and transfers the flight
# recorded LAST cycle ($1=flight number, $2=size), which the device then harvests
# + uploads alongside the backlog. The CURRENT cycle's flight is recorded now but
# is NOT transferred/uploadable until the NEXT power cycle (real DSU behavior).
# Durations come from the D_* globals (pre-computed in the loop for reproducibility).
run_cruise_cycle() {
    local avail="$1" avail_kb="$2"
    local pre=$(( USB_PRESENT < D_PRE ? USB_PRESENT : D_PRE ))
    local rem=$(( D_PRE - pre )); [ "$rem" -lt 1 ] && rem=1
    cell_set full; start_device
    cell_set full;  sleep "$pre"
    [ -n "$avail" ] && write_flight "$avail" "$avail_kb"    # DSU transfers last cycle's flight
    sleep "$rem"
    cell_set weak;  sleep "$D_TKO"
    cell_set off;   sleep "$D_CRZ"                          # in flight: no cell, flight being recorded
    cell_set flaky "$FLAKY_PCT"; sleep "$D_APP"
    cell_set full;  sleep "$D_TAXI"
    stop_device
}
run_ground_cycle() {
    local avail="$1" avail_kb="$2"
    local pre=$(( USB_PRESENT < D_GND ? USB_PRESENT : D_GND ))
    local rem=$(( D_GND - pre )); [ "$rem" -lt 1 ] && rem=1
    cell_set full; start_device
    sleep "$pre"
    [ -n "$avail" ] && write_flight "$avail" "$avail_kb"    # DSU transfers last cycle's flight
    sleep "$rem"
    stop_device
}

# ── Run ──────────────────────────────────────────────────────────────────────
log "═══════════════════════════════════════════════════════════════"
log "  Flight-cycle harvest benchmark (emulator)  seed=$SEED"
log "  backlog≈${BACKLOG_MB}MB cycles=$CYCLES cruise_prob=${CRUISE_PROB}% flaky=${FLAKY_PCT}% uplink=${UPLINK_KBPS}KB/s$([ "$FAST" = 1 ] && echo '  [FAST]')"
log "═══════════════════════════════════════════════════════════════"

stop_device 2>/dev/null || true
rm -rf "$SD_EMU"/* "$SD_INT"/* "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_modem.dat" "$EMU_CELL_FILE" 2>/dev/null || true
cleanup_s3; cleanup_aircraft_s3 "$SERIAL"
# Reset OTA latest.json to the emulator's own FW version, else every boot sees a
# newer target, downloads a doomed update (size-mismatch fail), and wastes
# per-cycle time + bandwidth that should go to the backlog. (e2e_unified does the
# same with src/main.cpp's version.)
EMU_FW=$(grep 'define FW_VERSION' "$FW_DIR/emu/main.cpp" | grep -o '"[^"]*"' | tr -d '"')
echo "{\"version\":\"$EMU_FW\",\"size\":0}" | aws s3 cp - "s3://$BUCKET/firmware/latest.json" --content-type application/json >/dev/null 2>&1
log "OTA reset to v$EMU_FW (skip doomed per-boot OTA download)"
cd "$FW_DIR" && ~/.local/bin/pio run -e emulator 2>&1 | tail -1

seed_backlog
echo "cycle,type,latest,hwm,backlog_flights,backlog_mb" > "$CSV"
echo "0,seed,$LATEST,0,$LATEST,$(backlog_mb_above 0)" >> "$CSV"

# PENDING = the flight recorded last cycle, transferred (written) THIS cycle when
# the DSU reconnects. Empty for cycle 1 (only the seeded backlog is available).
# LATEST = highest flight AVAILABLE to upload (excludes the in-flight current one).
auc_flights=0; auc_mb=0; catchup=-1; cycle=0
PENDING_FLIGHT=""; PENDING_KB=0
while [ "$cycle" -lt "$CYCLES" ]; do
    cycle=$((cycle+1))
    # Compute this cycle's scenario from the seeded parent RANDOM (order matters
    # for reproducibility): type, this-cycle's-flight size, then phase durations.
    if [ $((RANDOM % 100)) -lt "$CRUISE_PROB" ]; then typ=C; jit "$CRUISE_KB" 20; else typ=G; jit "$GROUND_KB" 30; fi
    newkb=$JIT
    jit "$T_PRETAKEOFF" 20; D_PRE=$JIT;  jit "$T_TAKEOFF" 20; D_TKO=$JIT
    jit "$T_CRUISE" 20;     D_CRZ=$JIT;  jit "$T_APPROACH" 20; D_APP=$JIT
    jit "$T_TAXI" 20;       D_TAXI=$JIT; jit "$T_GROUND" 20;   D_GND=$JIT
    log ""
    log "── cycle $cycle ($([ "$typ" = C ] && echo cruise || echo ground)); DSU transfers prev flight ${PENDING_FLIGHT:-none} ──"
    # Run the cycle: it writes the PREVIOUS cycle's flight (now transferred) at ~90s.
    if [ "$typ" = C ]; then run_cruise_cycle "$PENDING_FLIGHT" "$PENDING_KB"; else run_ground_cycle "$PENDING_FLIGHT" "$PENDING_KB"; fi
    # That flight is now available to upload.
    [ -n "$PENDING_FLIGHT" ] && LATEST="$PENDING_FLIGHT"
    # This cycle's flight is recorded now → transferred NEXT cycle (held pending).
    PENDING_FLIGHT=$((LATEST + 1)); PENDING_KB=$newkb; FSIZE[$PENDING_FLIGHT]=$newkb
    hwm=$(get_manifest_hwm "$SERIAL"); hwm=${hwm:-0}
    bf=$((LATEST - hwm)); bmb=$(backlog_mb_above "$hwm")
    auc_flights=$((auc_flights + bf)); auc_mb=$((auc_mb + bmb))
    echo "$cycle,$typ,$LATEST,$hwm,$bf,$bmb" >> "$CSV"
    log "  RESULT cycle=$cycle type=$typ latest=$LATEST hwm=$hwm backlog=${bf}flights/${bmb}MB"
    if [ "$hwm" -ge "$LATEST" ] && [ "$catchup" -lt 0 ]; then
        catchup=$cycle; log "  ★ CAUGHT UP at cycle $cycle"; break
    fi
done
stop_device 2>/dev/null || true

final_hwm=$(get_manifest_hwm "$SERIAL"); final_hwm=${final_hwm:-0}
log ""
log "═══════════════════════════════════════════════════════════════"
log "  METRIC (lower AUC = better harvest):"
log "    backlog AUC   = ${auc_flights} flight-cycles  /  ${auc_mb} MB-cycles"
log "    catchup_cycle = $catchup    (>0 = caught up)"
log "    final backlog = $((LATEST - final_hwm)) flights ($(backlog_mb_above "$final_hwm") MB)"
log "    seed=$SEED backlog≈${BACKLOG_MB}MB ran $cycle cycles   CSV: $CSV"
if [ "$catchup" -gt 0 ]; then pass "Caught up in $catchup cycles (AUC=${auc_flights}fc/${auc_mb}MBc)"
else fail "Not caught up in $cycle cycles (AUC=${auc_flights}fc/${auc_mb}MBc, final backlog $((LATEST-final_hwm)) flights)"; fi
log "═══════════════════════════════════════════════════════════════"
cleanup_aircraft_s3 "$SERIAL"
exit "$FAIL"

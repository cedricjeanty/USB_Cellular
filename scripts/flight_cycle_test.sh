#!/bin/bash
# AirBridge flight-cycle soak / harvest-optimization benchmark (EMULATOR ONLY).
#
# Simulates a NEW aircraft whose full flight-log backlog (~hundreds of MB to ~1 GB)
# lives in its DSU, and measures how effectively the device drains that backlog
# toward catch-up over repeated flight cycles on a scripted cellular profile.
# Designed for A/B optimization of the harvest/upload pipeline:
#
#   • BACKLOG LIVES IN THE DSU — the device starts EMPTY. A synthetic "aircraft
#     internal memory" .eaofh holds the backlog; the emulator's DSU thread streams
#     un-sent flights over USB into flightHistory/ RATE-LIMITED at SimDSU's
#     ~500 KB/s (the real USB transfer rate), one file per flight, starting at the
#     90s USB-present moment. The device harvests + uploads over cellular (~167
#     KB/s) concurrently. Both rates + the per-cycle connected time gate the drain.
#   • CURRENT FLIGHT TRANSFERS NEXT CYCLE — a flight is recorded during the flight
#     and only handed to the device on the NEXT power-on (the test appends it to the
#     DSU memory after the cycle). So each cycle uploads PAST flights only.
#   • SEEDED SCENARIOS — --seed makes everything that varies (cycle type, per-cycle
#     phase-duration jitter, per-flight sizes, the flaky drop pattern) deterministic.
#     Same seed ⇒ identical scenario ⇒ run-to-run comparable. A fixed SEED SET
#     (e.g. 1..N) is your benchmark suite; variability comes from the spread.
#   • PRIMARY METRIC — backlog AREA-UNDER-CURVE: Σ over cycles of the un-uploaded
#     backlog (flight-cycles AND MB-cycles). Always defined, lower = better harvest.
#     Secondary: cycles-to-catch-up + final backlog. A CSV trace is written.
#
# Cellular is scripted via EMU_CELL_FILE (full→weak→off→flaky→full); the DSU backlog
# via EMU_DSU_INTERNAL + EMU_DSU_DELAY_MS. Real uploads to S3 over pppd.
#
# Usage:
#   scripts/flight_cycle_test.sh --backlog-mb 200 --seed 1            # real soak (hours)
#   scripts/flight_cycle_test.sh --fast --backlog-mb 6 --seed 1       # quick validation
set -u

TARGET="emulator"
source "$(dirname "$0")/e2e_lib.sh"

# ── Params ───────────────────────────────────────────────────────────────────
SEED="${SEED:-1}"
BACKLOG_MB="${BACKLOG_MB:-200}"      # initial un-uploaded backlog target (MB), in the DSU
CYCLES="${CYCLES:-15}"
CRUISE_PROB="${CRUISE_PROB:-75}"     # % of cycles that are cruise (else quick-ground)
FLAKY_PCT="${FLAKY_PCT:-30}"
UPLINK_KBPS="${UPLINK_KBPS:-167}"    # emulator cellular uplink (167 ≈ real PCB); EMU_UPLINK_KBPS
USB_KBPS="${USB_KBPS:-500}"          # DSU→device USB transfer rate (SimDSU default); EMU_DSU_KBPS
CRUISE_KB="${CRUISE_KB:-10240}"      # mean cruise flight size (~10MB)
GROUND_KB="${GROUND_KB:-300}"        # mean quick-ground flight size
# Real-world phase-duration bases (seconds), jittered ±20% per cycle by the seed.
T_PRETAKEOFF="${T_PRETAKEOFF:-180}"; T_TAKEOFF="${T_TAKEOFF:-45}"
T_CRUISE="${T_CRUISE:-300}";         T_APPROACH="${T_APPROACH:-240}"
T_TAXI="${T_TAXI:-180}";             T_GROUND="${T_GROUND:-240}"
# USB-presentation / DSU-connect delay: the aircraft DSU only connects ~90s after
# power-on (the firmware holds D+ low until then), so the backlog transfer starts
# then and eats into the connected window — much of it for a short ground cycle.
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
        --usb-kbps)    USB_KBPS="$2"; shift 2 ;;
        --cruise-kb)   CRUISE_KB="$2"; shift 2 ;;
        --ground-kb)   GROUND_KB="$2"; shift 2 ;;
        --fast)        FAST=1; shift ;;   # compressed durations + fast links, for validation
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done
if [ "$FAST" = 1 ]; then
    # Phase durations compressed, BUT the 15s harvest quiet-window (QUIET_WINDOW_MS)
    # is a firmware constant the emulator does NOT compress — so a cycle must exceed
    # boot+connect (~10s) + quiet (15s) to harvest+upload anything. Ground cycles get
    # 30s for that reason (an 8s ground cycle uploads nothing and the backlog stalls).
    T_PRETAKEOFF=10; T_TAKEOFF=2; T_CRUISE=5; T_APPROACH=10; T_TAXI=12; T_GROUND=30
    UPLINK_KBPS="${UPLINK_KBPS_FAST:-4000}"
    USB_KBPS="${USB_KBPS_FAST:-8000}"     # fast USB so small flights transfer in <1s
    USB_PRESENT="${USB_PRESENT_FAST:-4}"
fi

EMU_CELL_FILE="$FW_DIR/emu_cell.ctl"; export EMU_CELL_FILE
export EMU_CELL_SEED="$SEED"          # seed the emulator's flaky-drop RNG too
export EMU_UPLINK_KBPS="$UPLINK_KBPS"
# DSU backlog: synthetic internal memory + the USB transfer thread (emu/main.cpp).
INTERNAL="$FW_DIR/dsu_memory.eaofh"
export EMU_DSU_INTERNAL="$INTERNAL"
export EMU_DSU_KBPS="$USB_KBPS"
export EMU_DSU_DELAY_MS=$((USB_PRESENT*1000))
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
RECORDED=0   # highest flight number recorded in the DSU internal memory

# ── Helpers ──────────────────────────────────────────────────────────────────
cell_set() {
    local word="$1" arg="${2:-}" tmp="${EMU_CELL_FILE}.tmp"
    if [ -n "$arg" ]; then echo "$word $arg" > "$tmp"; else echo "$word" > "$tmp"; fi
    mv -f "$tmp" "$EMU_CELL_FILE"; log "    cell → $word${arg:+ $arg}"
}

# Realistic ~3x-compressible, 0xEA-free filler: map random bytes onto a 5-symbol
# alphabet (A-E). gzip ratio ≈ 3.0x — matches real .eaofh logs (measured 2.95x on
# the 22MB fixture, 4.12x on the 1.49GB one) so the compression benchmark reflects
# the true on-the-wire saving. 0xEA never appears (A-E = 0x41-0x45) ⇒ buildFlightIndex
# sees exactly one boundary (the footer) per flight.
COMPRESS_SET="$(printf 'ABCDE%.0s' $(seq 52) | head -c 256)"

# Append one flight to the DSU internal memory: <size_kb> of compressible filler
# followed by a 0x4C footer carrying the flight number. The block buildFlightIndex
# assigns to flight N is [prev footer end, this footer end) = its filler + footer,
# so the slice the DSU transfers contains a valid last-record (lastRecordFromLog → N).
append_flight() {
    local flight="$1" size_kb="$2"; local fill=$size_kb
    [ "$fill" -lt 1 ] && fill=1
    head -c $((fill*1024)) /dev/urandom | tr '\000-\377' "$COMPRESS_SET" >> "$INTERNAL"
    append_eaofh_trailer "$INTERNAL" "$SERIAL" "$flight"
    FSIZE[$flight]=$size_kb
}

# MB of backlog above a given hwm (sum of un-uploaded flight sizes, up to LATEST).
backlog_mb_above() {
    local hwm="$1" f kb=0
    for f in $(seq $((hwm+1)) "$LATEST"); do kb=$((kb + ${FSIZE[$f]:-0})); done
    echo $((kb/1024))
}

# Build the initial DSU backlog: discrete flights until ~BACKLOG_MB, mostly
# cruise-sized. The device starts EMPTY — this only writes the DSU's memory.
build_backlog() {
    : > "$INTERNAL"
    local total_kb=0 target_kb=$((BACKLOG_MB*1024)) f=0
    while [ "$total_kb" -lt "$target_kb" ]; do
        f=$((f+1))
        if [ $((RANDOM % 100)) -lt "$CRUISE_PROB" ]; then jit "$CRUISE_KB" 20; else jit "$GROUND_KB" 30; fi
        append_flight "$f" "$JIT"; total_kb=$((total_kb+JIT))
    done
    RECORDED=$f
    log "DSU backlog: $RECORDED flights ≈ $((total_kb/1024))MB in $(basename "$INTERNAL") @${USB_KBPS}KB/s USB (device starts empty)"
}

# A cycle is just the cellular timeline; the emulator's DSU thread autonomously
# connects EMU_DSU_DELAY_MS (≈90s) after boot and streams the backlog rate-limited,
# interrupted at power-off (stop_device) with already-transferred flights intact.
# Durations come from the D_* globals (pre-computed in the loop for reproducibility).
run_cruise_cycle() {
    cell_set full; start_device                  # DSU connects at +USB_PRESENT, streams backlog
    sleep "$D_PRE"                               # pre-takeoff: full signal, USB transfer + upload
    cell_set weak;  sleep "$D_TKO"               # takeoff: degrading
    cell_set off;   sleep "$D_CRZ"               # cruise: no cell (flight being recorded)
    cell_set flaky "$FLAKY_PCT"; sleep "$D_APP"  # approach: flaky link, partial uploads + retries
    cell_set full;  sleep "$D_TAXI"              # taxi: clean catch-up window
    stop_device
}
run_ground_cycle() {
    cell_set full; start_device
    sleep "$D_GND"                               # brief ground power-up: full signal throughout
    stop_device
}

# ── Run ──────────────────────────────────────────────────────────────────────
log "═══════════════════════════════════════════════════════════════"
log "  Flight-cycle harvest benchmark (emulator)  seed=$SEED"
log "  backlog≈${BACKLOG_MB}MB cycles=$CYCLES cruise_prob=${CRUISE_PROB}% flaky=${FLAKY_PCT}% uplink=${UPLINK_KBPS}KB/s usb=${USB_KBPS}KB/s$([ "$FAST" = 1 ] && echo '  [FAST]')"
log "═══════════════════════════════════════════════════════════════"

stop_device 2>/dev/null || true
rm -rf "$SD_EMU"/* "$SD_INT"/* "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_modem.dat" "$EMU_CELL_FILE" "$INTERNAL" 2>/dev/null || true
cleanup_s3; cleanup_aircraft_s3 "$SERIAL"
# Reset OTA latest.json to the emulator's own FW version, else every boot sees a
# newer target, downloads a doomed update (size-mismatch fail), and wastes
# per-cycle time + bandwidth that should go to the backlog. (e2e_unified does the
# same with src/main.cpp's version.)
EMU_FW=$(grep 'define FW_VERSION' "$FW_DIR/emu/main.cpp" | grep -o '"[^"]*"' | tr -d '"')
echo "{\"version\":\"$EMU_FW\",\"size\":0}" | aws s3 cp - "s3://$BUCKET/firmware/latest.json" --content-type application/json >/dev/null 2>&1
log "OTA reset to v$EMU_FW (skip doomed per-boot OTA download)"
cd "$FW_DIR" && ~/.local/bin/pio run -e emulator 2>&1 | tail -1

build_backlog
echo "cycle,type,dsu_latest,hwm,backlog_flights,backlog_mb" > "$CSV"
LATEST=$RECORDED; echo "0,seed,$RECORDED,0,$RECORDED,$(backlog_mb_above 0)" >> "$CSV"

# Each cycle: LATEST = flights the DSU can transfer this cycle (all recorded so
# far). The just-flown flight is appended AFTER the cycle → transfers next power-on.
# catch-up = device hwm reaches LATEST (everything transferable has been uploaded).
auc_flights=0; auc_mb=0; catchup=-1; cycle=0
while [ "$cycle" -lt "$CYCLES" ]; do
    cycle=$((cycle+1))
    LATEST=$RECORDED
    # Compute this cycle's scenario from the seeded parent RANDOM (order matters
    # for reproducibility): type, this-cycle's-flight size, then phase durations.
    if [ $((RANDOM % 100)) -lt "$CRUISE_PROB" ]; then typ=C; jit "$CRUISE_KB" 20; else typ=G; jit "$GROUND_KB" 30; fi
    newkb=$JIT
    jit "$T_PRETAKEOFF" 20; D_PRE=$JIT;  jit "$T_TAKEOFF" 20; D_TKO=$JIT
    jit "$T_CRUISE" 20;     D_CRZ=$JIT;  jit "$T_APPROACH" 20; D_APP=$JIT
    jit "$T_TAXI" 20;       D_TAXI=$JIT; jit "$T_GROUND" 20;   D_GND=$JIT
    log ""
    log "── cycle $cycle ($([ "$typ" = C ] && echo cruise || echo ground)); DSU holds $LATEST flights ──"
    if [ "$typ" = C ]; then run_cruise_cycle; else run_ground_cycle; fi
    hwm=$(get_manifest_hwm "$SERIAL"); hwm=${hwm:-0}
    bf=$((LATEST - hwm)); bmb=$(backlog_mb_above "$hwm")
    auc_flights=$((auc_flights + bf)); auc_mb=$((auc_mb + bmb))
    echo "$cycle,$typ,$LATEST,$hwm,$bf,$bmb" >> "$CSV"
    log "  RESULT cycle=$cycle type=$typ dsu_latest=$LATEST hwm=$hwm backlog=${bf}flights/${bmb}MB"
    # Record THIS cycle's flight into the DSU → transferred on the NEXT power-on.
    RECORDED=$((RECORDED+1)); append_flight "$RECORDED" "$newkb"
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

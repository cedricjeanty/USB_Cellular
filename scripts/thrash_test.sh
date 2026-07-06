#!/bin/bash
# AirBridge power-cut THRASH / chaos soak (EMULATOR ONLY).
#
# Goal: try to WEDGE the device. Present a DSU backlog, then repeatedly cut power
# at RANDOM uptimes (default 1s..5min) and verify the unit (a) never gets stuck and
# (b) eventually gets every flight up to S3. Each cut lands wherever it lands — in
# the 90s pre-USB window, mid-USB-transfer (leaving a truncated .eaofh), mid-harvest,
# or mid-multipart-upload — exercising the boot-recovery + multipart-resume paths as
# hard as possible.
#
# WHY THE EMULATOR CAN CATCH A WEDGE:
#   • host-dir SD (default backend), NVS (emu_nvs.dat) and multipart resume state all
#     PERSIST across a kill -9, exactly like flash/SD survive a brownout — so resume
#     is genuinely exercised, not faked.
#   • the DSU transfer thread resumes from the cookie / highest flightHistory file each
#     new process, so a cut mid-stream is picked up on the next boot.
#   • S3 manifest high-water-mark is the authoritative "logs actually landed" signal.
#
# STUCK DETECTION (any ⇒ FAIL, run halts with forensics):
#   1. BOOT HANG   — a fresh process never logs "Init complete" within the boot budget.
#   2. NO PROGRESS — once at least one flight has landed on the SD (real upload work is
#      available), cumulative post-boot uptime with ZERO manifest-hwm advance exceeds
#      --stuck-uptime (default 1200s). Normally hwm advances every cycle or two.
#   3. SELF-EXIT   — the emu process dies on its own before we cut it (firmware abort);
#      counted, and a burst of them is flagged.
#   Anomaly markers (WATCHDOG / SAFE MODE / PANIC / abort) are scanned each cycle and
#   surfaced — a pure power-cut soak should never trip the crash-loop firewall
#   (power cut ⇒ POWERON reset ⇒ decideBootMode stays NORMAL).
#
# NOTE: the emulator does NOT model the dbg/boots crash-loop counter (only EMU_SAFE_MODE
# forces Safe Mode), so it faithfully treats every power cut as a clean POWERON boot.
#
# Usage:
#   scripts/thrash_test.sh                                  # 200MB, cuts 1s..300s (hours)
#   scripts/thrash_test.sh --fast                           # tiny+fast harness self-check
#   scripts/thrash_test.sh --backlog-mb 200 --max-cut 300 --seed 1
set -u

TARGET="emulator"
source "$(dirname "$0")/e2e_lib.sh"

# ── Params ───────────────────────────────────────────────────────────────────
SEED="${SEED:-1}"
BACKLOG_MB="${BACKLOG_MB:-200}"      # DSU backlog to drain to S3 (MB)
FLIGHT_KB="${FLIGHT_KB:-10240}"      # mean flight size (~10MB ⇒ multipart resume stress)
MIN_CUT="${MIN_CUT:-1}"              # min uptime before a power cut (s)
MAX_CUT="${MAX_CUT:-300}"            # max uptime before a power cut (s)  (5 min)
OFF_S="${OFF_S:-1}"                  # power-off duration between cuts (s)
FLAKY_PCT="${FLAKY_PCT:-0}"          # optional cellular flakiness (0 = clean link)
UPLINK_KBPS="${UPLINK_KBPS:-167}"    # cellular uplink (167 ≈ real PCB)
USB_KBPS="${USB_KBPS:-500}"          # DSU→device USB transfer rate (SimDSU default)
USB_PRESENT="${USB_PRESENT:-90}"     # USB-present / DSU-connect delay (real 90s)
MAX_CYCLES="${MAX_CYCLES:-400}"      # safety cap on power-cut cycles
MAX_WALL_MIN="${MAX_WALL_MIN:-360}"  # safety cap on wall-clock (minutes)
STUCK_UPTIME="${STUCK_UPTIME:-1200}" # cumulative uptime w/ no hwm advance ⇒ stuck (s)
BOOT_BUDGET="${BOOT_BUDGET:-45}"     # seconds a fresh process gets to log "Init complete"
FAST=0

while [ $# -gt 0 ]; do
    case "$1" in
        --seed)         SEED="$2"; shift 2 ;;
        --backlog-mb)   BACKLOG_MB="$2"; shift 2 ;;
        --flight-kb)    FLIGHT_KB="$2"; shift 2 ;;
        --min-cut)      MIN_CUT="$2"; shift 2 ;;
        --max-cut)      MAX_CUT="$2"; shift 2 ;;
        --off)          OFF_S="$2"; shift 2 ;;
        --flaky)        FLAKY_PCT="$2"; shift 2 ;;
        --uplink-kbps)  UPLINK_KBPS="$2"; shift 2 ;;
        --usb-kbps)     USB_KBPS="$2"; shift 2 ;;
        --usb-present)  USB_PRESENT="$2"; shift 2 ;;
        --max-cycles)   MAX_CYCLES="$2"; shift 2 ;;
        --max-wall-min) MAX_WALL_MIN="$2"; shift 2 ;;
        --stuck-uptime) STUCK_UPTIME="$2"; shift 2 ;;
        --compress)     export EMU_COMPRESS=1; shift ;;
        --fast)         FAST=1; shift ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done
if [ "$FAST" = 1 ]; then
    # Harness self-check: enough backlog + slow-enough links that cuts genuinely land
    # mid-USB-transfer and mid-multipart-upload (each start_device blocks ~15s, so the
    # backlog must take much longer than that to drain). Compressed USB-present.
    BACKLOG_MB="${BACKLOG_MB_FAST:-40}"
    FLIGHT_KB="${FLIGHT_KB_FAST:-8192}"
    UPLINK_KBPS="${UPLINK_KBPS_FAST:-700}"
    USB_KBPS="${USB_KBPS_FAST:-1500}"
    USB_PRESENT="${USB_PRESENT_FAST:-4}"
    MIN_CUT="${MIN_CUT_FAST:-3}"
    MAX_CUT="${MAX_CUT_FAST:-30}"
    STUCK_UPTIME="${STUCK_UPTIME_FAST:-240}"
    MAX_WALL_MIN="${MAX_WALL_MIN_FAST:-30}"
fi

export EMU_UPLINK_KBPS="$UPLINK_KBPS"
INTERNAL="$FW_DIR/dsu_memory.eaofh"
export EMU_DSU_INTERNAL="$INTERNAL"
export EMU_DSU_KBPS="$USB_KBPS"
export EMU_DSU_DELAY_MS=$((USB_PRESENT*1000))
EMU_CELL_FILE="$FW_DIR/emu_cell.ctl"; export EMU_CELL_FILE
export EMU_CELL_SEED="$SEED"
CSV="/tmp/thrash_seed${SEED}.csv"
STUCKLOG="/tmp/thrash_seed${SEED}_stuck.log"
PASS=0; FAIL=0; SKIP=0
RANDOM="$SEED"

# ── Backlog builder (compressible, one 0x4C footer per flight) ────────────────
COMPRESS_SET="$(printf 'ABCDE%.0s' $(seq 52) | head -c 256)"
JIT=0
jit() { local base=$1 pct=$2; local d=$((base*pct/100)); [ "$d" -lt 1 ] && d=1; JIT=$((base - d + RANDOM % (2*d+1))); }
append_flight() {
    local flight="$1" size_kb="$2"; local fill=$size_kb
    [ "$fill" -lt 1 ] && fill=1
    head -c $((fill*1024)) /dev/urandom | tr '\000-\377' "$COMPRESS_SET" >> "$INTERNAL"
    append_eaofh_trailer "$INTERNAL" "$SERIAL" "$flight"
}
build_backlog() {
    : > "$INTERNAL"
    local total_kb=0 target_kb=$((BACKLOG_MB*1024)) f=0
    while [ "$total_kb" -lt "$target_kb" ]; do
        f=$((f+1)); jit "$FLIGHT_KB" 25
        append_flight "$f" "$JIT"; total_kb=$((total_kb+JIT))
    done
    LATEST=$f
    log "DSU backlog: $LATEST flights ≈ $((total_kb/1024))MB @${USB_KBPS}KB/s USB (device starts EMPTY)"
}

# Files still sitting on the SD (transferred but not yet uploaded) — tells us whether
# real upload work is AVAILABLE (gates the no-progress stuck check).
sd_pending_files() {
    { ls "$SD_EMU/flightHistory/" 2>/dev/null; ls -d "$SD_INT"/upload/*/ 2>/dev/null; } | wc -l | tr -d ' '
}

anomaly_scan() {  # after_line — echo any wedge/firewall markers seen since a mark
    tail -n "+$(( ${1:-0} + 1 ))" "$E2E_LOG" 2>/dev/null \
        | grep -Eio "WATCHDOG[^ ]*|SAFE MODE|PANIC|abort\(\)|Guru Meditation|assert failed" \
        | sort -u | tr '\n' ' '
}

# ── Setup ─────────────────────────────────────────────────────────────────────
log "═══════════════════════════════════════════════════════════════"
log "  POWER-CUT THRASH  seed=$SEED  backlog≈${BACKLOG_MB}MB"
log "  cuts=random(${MIN_CUT}..${MAX_CUT})s  uplink=${UPLINK_KBPS}KB/s usb=${USB_KBPS}KB/s"
log "  usb_present=${USB_PRESENT}s flaky=${FLAKY_PCT}%  stuck_uptime=${STUCK_UPTIME}s$([ "$FAST" = 1 ] && echo '  [FAST]')"
log "═══════════════════════════════════════════════════════════════"

stop_device 2>/dev/null || true
rm -rf "$SD_EMU"/* "$SD_INT"/* "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_modem.dat" "$EMU_CELL_FILE" "$INTERNAL" 2>/dev/null || true
cleanup_s3; cleanup_aircraft_s3 "$SERIAL"
EMU_FW=$(grep 'define FW_VERSION' "$FW_DIR/emu/main.cpp" | grep -o '"[^"]*"' | tr -d '"')
echo "{\"version\":\"$EMU_FW\",\"size\":0}" | aws s3 cp - "s3://$BUCKET/firmware/latest.json" --content-type application/json >/dev/null 2>&1
log "OTA reset to v$EMU_FW (skip doomed per-boot OTA download)"
# Build ONCE up front — never rebuild during the run (would clobber the live binary).
cd "$FW_DIR" && ~/.local/bin/pio run -e emulator 2>&1 | tail -1
[ -x "$EMU" ] || { log "FATAL: emulator binary missing ($EMU)"; exit 1; }

# Set the cellular profile before the first boot (clean full link, or flaky if asked).
if [ "$FLAKY_PCT" -gt 0 ]; then echo "flaky $FLAKY_PCT" > "$EMU_CELL_FILE"; else echo "full" > "$EMU_CELL_FILE"; fi

build_backlog
echo "cycle,window_s,uptime_s,hwm,latest,pending,progress,anomalies" > "$CSV"

# ── Thrash loop ────────────────────────────────────────────────────────────────
prev_hwm=0; cum_no_progress=0; self_exits=0; recent_self_exits=0
cycle=0; caught_up=0; stuck=0; boot_hang=0
start_wall=$(date +%s)
max_wall_s=$((MAX_WALL_MIN*60))

while :; do
    now=$(date +%s); elapsed=$((now-start_wall))
    hwm_now=$(get_manifest_hwm "$SERIAL"); hwm_now=${hwm_now:-0}
    if [ "$hwm_now" -ge "$LATEST" ]; then caught_up=1; break; fi
    [ "$cycle" -ge "$MAX_CYCLES" ] && { log "  reached --max-cycles=$MAX_CYCLES"; break; }
    [ "$elapsed" -ge "$max_wall_s" ] && { log "  reached --max-wall-min=$MAX_WALL_MIN"; break; }

    cycle=$((cycle+1))
    # Random uptime for THIS cycle before we cut power.
    span=$((MAX_CUT - MIN_CUT + 1)); [ "$span" -lt 1 ] && span=1
    window=$((MIN_CUT + RANDOM % span))

    # start_device TRUNCATES the emu log per launch, so this cycle's log IS this boot:
    # scan from line 0 (a pre-start mark would point past the freshly truncated log).
    start_device
    # BOOT HANG: a fresh process must reach "Init complete" within the boot budget.
    if ! wait_for_log "$(marker init)" 0 "$BOOT_BUDGET" "init (boot $cycle)"; then
        boot_hang=1; stuck=1
        log "  ★ STUCK: boot hang — no 'Init complete' in ${BOOT_BUDGET}s (cycle $cycle)"
        break
    fi
    t0=$(date +%s)

    # Run for the window, but notice an unexpected self-exit (firmware abort).
    died=0
    while :; do
        el=$(( $(date +%s) - t0 )); [ "$el" -ge "$window" ] && break
        if [ -n "$EMU_PID" ] && ! kill -0 "$EMU_PID" 2>/dev/null; then died=1; break; fi
        sleep 1
    done
    uptime_s=$(( $(date +%s) - t0 ))

    if [ "$died" = 1 ]; then
        self_exits=$((self_exits+1)); recent_self_exits=$((recent_self_exits+1))
        log "  ! self-exit: emu process died on its own after ${uptime_s}s (cycle $cycle)"
        EMU_PID=""
    else
        recent_self_exits=0
        power_cut "$OFF_S"
    fi

    hwm=$(get_manifest_hwm "$SERIAL"); hwm=${hwm:-0}
    pending=$(sd_pending_files)
    anom=$(anomaly_scan 0)
    progress=0
    if [ "$hwm" -gt "$prev_hwm" ]; then
        progress=$((hwm - prev_hwm)); prev_hwm=$hwm; cum_no_progress=0
    else
        # Only hold it against the firmware once real upload work is on the SD.
        if [ "$pending" -gt 0 ]; then cum_no_progress=$((cum_no_progress + uptime_s)); fi
    fi
    echo "$cycle,$window,$uptime_s,$hwm,$LATEST,$pending,$progress,${anom// /|}" >> "$CSV"
    exitnote=""; [ "$died" = 1 ] && exitnote="  [self-exit]"
    log "  cycle $cycle: window=${window}s up=${uptime_s}s hwm=$hwm/$LATEST pending=$pending progress=+$progress no_prog_uptime=${cum_no_progress}s${anom:+  ANOMALY: $anom}$exitnote"

    # NO-PROGRESS stuck check.
    if [ "$cum_no_progress" -ge "$STUCK_UPTIME" ]; then
        stuck=1
        log "  ★ STUCK: ${cum_no_progress}s of uptime with pending work and NO hwm advance (cycle $cycle)"
        break
    fi
    # Burst of self-exits = a crash loop, not power thrash.
    if [ "$recent_self_exits" -ge 5 ]; then
        stuck=1
        log "  ★ STUCK: $recent_self_exits consecutive firmware self-exits (crash loop)"
        break
    fi
done

# ── Verdict ────────────────────────────────────────────────────────────────────
stop_device 2>/dev/null || true
final_hwm=$(get_manifest_hwm "$SERIAL"); final_hwm=${final_hwm:-0}
s3_objs=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null | grep -c '\.eaofh')
wall=$(( ($(date +%s) - start_wall) / 60 ))
log ""
log "═══════════════════════════════════════════════════════════════"
log "  THRASH RESULT  seed=$SEED"
log "    cycles=$cycle  wall≈${wall}min  self_exits=$self_exits"
log "    final hwm=$final_hwm / $LATEST   S3 .eaofh objects=$s3_objs"
log "    CSV: $CSV"

if [ "$stuck" = 1 ]; then
    # Archive the last process's log for forensics.
    cp -f "$E2E_LOG" "$STUCKLOG" 2>/dev/null
    log "    forensic log: $STUCKLOG"
    if [ "$boot_hang" = 1 ]; then fail "WEDGED (boot hang) at cycle $cycle — hwm=$final_hwm/$LATEST"
    else fail "WEDGED (no progress) — hwm=$final_hwm/$LATEST after $cycle cycles"; fi
elif [ "$caught_up" = 1 ]; then
    if [ "$final_hwm" -ge "$LATEST" ] && [ "$s3_objs" -ge "$LATEST" ]; then
        pass "Drained all $LATEST flights to S3 under power-cut thrash ($cycle cycles, ~${wall}min, $self_exits self-exits)"
    else
        fail "hwm caught up ($final_hwm) but S3 has only $s3_objs/$LATEST .eaofh objects"
    fi
else
    fail "Did not catch up (hwm=$final_hwm/$LATEST) within limits — not declared stuck, but incomplete"
fi
log "═══════════════════════════════════════════════════════════════"
cleanup_aircraft_s3 "$SERIAL"
exit "$FAIL"

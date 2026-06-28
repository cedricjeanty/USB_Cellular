#!/bin/bash
# AirBridge flight-cycle HARDWARE soak — real device, real cellular (DEVICE ONLY).
#
# The on-hardware analogue of flight_cycle_test.sh: seed a backlog in the DSU
# (host_dsu.py writes flights to the device's USB volume), then run a series of
# ~fixed-length power cycles and measure how many it takes to drain the backlog to
# S3 over REAL cellular. Validates the full pipeline + on-device gzip (ROM miniz
# tdefl) end-to-end, and measures the cellular bytes billed (~$0.03/MB).
#
# Differences from the emulator test (by design):
#   • Real cellular — no scripted full/weak/off/flaky. The SIM7600 sees whatever
#     Hologram gives it. So this measures real-cell throughput + the gzip win, not
#     the signal-loss resilience (the emulator covers that).
#   • DSU = host_dsu.py mounting the USB volume at the 90s presentation.
#   • Compression (--compress) gzips .eaofh on-device; the seeded bodies are
#     ~3x-compressible (host_dsu --compressible) so the ratio matches real logs.
#
# Cost: only the COMPRESSED upload bytes are billed. A 200MB backlog ⇒ ~67MB ⇒ ~$2.
#
# Usage (run with the device + CoolGear hub attached):
#   scripts/flight_cycle_hw.sh --backlog-mb 200 --flight-kb 8192 --compress
#   scripts/flight_cycle_hw.sh --backlog-mb 16 --flight-kb 2048 --compress   # cheap smoke
set -u

TARGET="device"
source "$(dirname "$0")/e2e_lib.sh"

# ── Params ───────────────────────────────────────────────────────────────────
BACKLOG_MB="${BACKLOG_MB:-200}"
FLIGHT_KB="${FLIGHT_KB:-8192}"        # mean flight size (~8MB, like the emulator)
COMPRESS=0                            # --compress → gzip on-device
CYCLE_SECS="${CYCLE_SECS:-480}"       # upload window per power cycle (~8 min)
MAX_CYCLES="${MAX_CYCLES:-12}"
SERIAL="${SERIAL:-EA500.E2ETES}"      # test serial (never collides with fleet data)

while [ $# -gt 0 ]; do
    case "$1" in
        --backlog-mb)  BACKLOG_MB="$2"; shift 2 ;;
        --flight-kb)   FLIGHT_KB="$2"; shift 2 ;;
        --compress)    COMPRESS=1; shift ;;
        --cycle-secs)  CYCLE_SECS="$2"; shift 2 ;;
        --max-cycles)  MAX_CYCLES="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

NFLIGHTS=$(( (BACKLOG_MB * 1024 + FLIGHT_KB - 1) / FLIGHT_KB ))
PASS=0; FAIL=0; SKIP=0
HOSTDSU="python3 $HOME/USBCellular/scripts/host_dsu.py"
CSV="/tmp/flight_cycle_hw.csv"

# Find the device's USB-visible block partition (P1). Mirrors e2e_lib's scan.
find_usb_part() {
    local w d
    for w in $(seq 1 60); do
        for d in /dev/sda1 /dev/sdb1 /dev/sdc1 /dev/sdd1; do [ -b "$d" ] && { echo "$d"; return 0; }; done
        sleep 1
    done
    return 1
}

# Write a one-line airbridge.cmd to the USB volume (processed by check_p1_magic at
# the NEXT boot — so compression is active from the first harvest). Also (re)plants
# the cookie if $2 is given.
write_cmd_and_cookie() {
    local directive="$1" cookie="${2:-}"
    local dev; dev=$(find_usb_part) || { log "  no USB partition to write cmd"; return 1; }
    sudo mount -o noatime "$dev" /mnt 2>/dev/null || { log "  mount failed"; return 1; }
    printf '%s\n' "$directive" | sudo tee /mnt/airbridge.cmd >/dev/null
    [ -n "$cookie" ] && $HOSTDSU --mount /mnt --serial "$SERIAL" --plant-cookie "$cookie" >/dev/null
    sync; sudo umount /mnt 2>/dev/null
    log "  airbridge.cmd='$directive'${cookie:+ cookie=$cookie}"
}

# Seed the backlog: mount once, write flights 1..NFLIGHTS (compressible) past a
# cookie of 0. host_dsu --mount avoids a per-flight remount.
seed_backlog_hw() {
    local dev; dev=$(find_usb_part) || { fail "no USB partition to seed"; return 1; }
    sudo mount -o noatime "$dev" /mnt 2>/dev/null || { fail "seed mount failed"; return 1; }
    $HOSTDSU --mount /mnt --serial "$SERIAL" --plant-cookie 0 >/dev/null
    local f
    for f in $(seq 1 "$NFLIGHTS"); do
        $HOSTDSU --mount /mnt --serial "$SERIAL" --max-flight "$f" --size-kb "$FLIGHT_KB" \
                 --compressible --ignore-cookie >/dev/null
    done
    sync; sudo umount /mnt 2>/dev/null
    log "  Seeded $NFLIGHTS flights (~${BACKLOG_MB}MB, ~3x-compressible) onto USB volume"
}

# Total MB stored in S3 for this aircraft (the cellular bytes billed).
s3_uploaded_mb() {
    aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null \
        | awk '{s+=$3} END{printf "%.1f", s/1048576}'
}

# ── Run ──────────────────────────────────────────────────────────────────────
log "═══════════════════════════════════════════════════════════════"
log "  Flight-cycle HARDWARE soak  serial=$SERIAL"
log "  backlog≈${BACKLOG_MB}MB ($NFLIGHTS flights @${FLIGHT_KB}KB) compress=$COMPRESS cycle=${CYCLE_SECS}s"
log "═══════════════════════════════════════════════════════════════"

command -v aws >/dev/null || { fail "aws CLI required"; exit 1; }
cleanup_aircraft_s3 "$SERIAL"
echo "cycle,hwm,backlog_flights,s3_mb" > "$CSV"

# Setup boot: drop the compress directive + cookie BEFORE seeding, so check_p1_magic
# enables compression from the first harvest. Power-cycle so boot processes it.
log "── setup: enable compression + reset cookie ──"
start_device
[ "$COMPRESS" = 1 ] && write_cmd_and_cookie "compress on" 0 || write_cmd_and_cookie "compress off" 0
stop_device; sleep 3

auc=0; catchup=-1; cycle=0
while [ "$cycle" -lt "$MAX_CYCLES" ]; do
    cycle=$((cycle+1))
    log ""
    log "── cycle $cycle ──"
    start_device
    if [ "$cycle" = 1 ]; then
        seed_backlog_hw                       # DSU writes the whole backlog at first connect
    fi
    # Let the device harvest (gzip) + upload over real cell for the cycle window.
    sleep "$CYCLE_SECS"
    stop_device

    hwm=$(get_manifest_hwm "$SERIAL"); hwm=${hwm:-0}
    bf=$((NFLIGHTS - hwm)); [ "$bf" -lt 0 ] && bf=0
    mb=$(s3_uploaded_mb)
    auc=$((auc + bf))
    echo "$cycle,$hwm,$bf,$mb" >> "$CSV"
    log "  RESULT cycle=$cycle hwm=$hwm backlog=${bf}flights s3=${mb}MB"
    if [ "$hwm" -ge "$NFLIGHTS" ] && [ "$catchup" -lt 0 ]; then
        catchup=$cycle; log "  ★ CAUGHT UP at cycle $cycle"; break
    fi
done

final_mb=$(s3_uploaded_mb)
cost=$(awk -v m="$final_mb" 'BEGIN{printf "%.2f", m*0.03}')
log ""
log "═══════════════════════════════════════════════════════════════"
log "  HARDWARE RESULT:"
log "    catchup_cycle = $catchup    (>0 = caught up)"
log "    backlog AUC   = $auc flight-cycles"
log "    S3 uploaded   = ${final_mb}MB  (cellular cost ≈ \$$cost)"
log "    backlog/flights=$NFLIGHTS compress=$COMPRESS  CSV: $CSV"
log "═══════════════════════════════════════════════════════════════"
if [ "$catchup" -gt 0 ]; then pass "Caught up in $catchup cycles, ${final_mb}MB billed"
else fail "Not caught up in $cycle cycles (${final_mb}MB billed)"; fi
exit "$FAIL"

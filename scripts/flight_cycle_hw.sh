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
CYCLE_SECS="${CYCLE_SECS:-300}"       # power-on window per cycle (~5 min) → cut power every 5 min
FEED_GAP="${FEED_GAP:-20}"            # pause between DSU flight writes (lets the device harvest)
MAX_CYCLES="${MAX_CYCLES:-20}"
SERIAL="${SERIAL:-EA500.E2ETES}"      # test serial (never collides with fleet data)

while [ $# -gt 0 ]; do
    case "$1" in
        --backlog-mb)  BACKLOG_MB="$2"; shift 2 ;;
        --flight-kb)   FLIGHT_KB="$2"; shift 2 ;;
        --compress)    COMPRESS=1; shift ;;
        --feed-gap)    FEED_GAP="$2"; shift 2 ;;
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
# Wait 150s: when the modem's OTA+cookie check is slow, USB presentation falls back
# to the full 90s delay, so a 60s wait raced ahead of it and the seed/cmd-write failed.
find_usb_part() {
    local w d
    for w in $(seq 1 150); do
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
    sudo mount -o noatime,uid=$(id -u),gid=$(id -g) "$dev" /mnt 2>/dev/null || { log "  mount failed"; return 1; }
    printf '%s\n' "$directive" | sudo tee /mnt/airbridge.cmd >/dev/null
    [ -n "$cookie" ] && $HOSTDSU --mount /mnt --serial "$SERIAL" --plant-cookie "$cookie" >/dev/null
    sync; sudo umount /mnt 2>/dev/null
    log "  airbridge.cmd='$directive'${cookie:+ cookie=$cookie}"
}

# ── Interleaved DSU feeder ─────────────────────────────────────────────────
# Realistic model: the aircraft DSU dribbles flights onto the USB volume WHILE the
# device harvests+uploads, and power is cut every $CYCLE_SECS mid-stream. So we do NOT
# seed up front — a background feeder writes one flight at a time (mount → write →
# UNMOUNT so the device gets a USB-quiet window to harvest → pause), advancing across
# power cycles via a persistent cursor. A cut lands on whatever's live: a half-written
# flight (TRANSFER phase) or an in-flight S3 part (UPLOAD phase). host_dsu is atomic
# (.part+rename) so a torn write leaves no half-flight; the cursor re-writes it next cycle.
CURSOR=/tmp/soak_next_flight
FEEDON=/tmp/soak_feed_on

# Returns: 0=written, 1=device gone (power cut), 2=transient (device busy harvesting).
# The device briefly drops/holds its USB volume while it harvests each flight, so a
# failed mount is usually transient — the caller retries rather than quitting the cycle.
feed_one_flight() {   # $1 = flight number
    local f="$1" dev d w
    dev=""
    for w in $(seq 1 8); do
        for d in /dev/sda1 /dev/sdb1 /dev/sdc1 /dev/sdd1; do [ -b "$d" ] && { dev="$d"; break; }; done
        [ -n "$dev" ] && break
        sleep 1
    done
    [ -z "$dev" ] && return 1                          # no USB → device is off (cut)
    sudo mount -o noatime,uid=$(id -u),gid=$(id -g) "$dev" /mnt 2>/dev/null || return 2   # busy → retry
    if ! $HOSTDSU --mount /mnt --serial "$SERIAL" --max-flight "$f" --size-kb "$FLIGHT_KB" \
                  --compressible --ignore-cookie >/dev/null 2>&1; then
        sync 2>/dev/null; sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null
        return 2
    fi
    sync 2>/dev/null; sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null
    return 0
}

run_feeder() {   # background: keep the device continuously fed so cuts land mid-work + a backlog builds
    local misses=0
    while [ -f "$FEEDON" ]; do
        local f; f=$(cat "$CURSOR" 2>/dev/null || echo 1)
        [ "$f" -gt "$NFLIGHTS" ] && break             # whole backlog transferred
        feed_one_flight "$f"; local rc=$?
        if [ "$rc" = 0 ]; then
            echo $((f+1)) > "$CURSOR"; log "  fed flight $f/$NFLIGHTS"; misses=0
            local g=0; while [ -f "$FEEDON" ] && [ "$g" -lt "$FEED_GAP" ]; do sleep 1; g=$((g+1)); done
        elif [ "$rc" = 1 ]; then
            misses=$((misses+1)); [ "$misses" -ge 20 ] && break   # ~20s no USB → power was cut
            sleep 1
        else
            sleep 3                                    # device busy harvesting — retry same flight
        fi
    done
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
echo "cycle,hwm,inflight_backlog,s3_mb" > "$CSV"

# Setup boot: drop the compress directive + cookie BEFORE seeding, so check_p1_magic
# enables compression from the first harvest. Power-cycle so boot processes it.
log "── setup: enable compression + reset cookie ──"
echo 1 > "$CURSOR"; rm -f "$FEEDON"
start_device
[ "$COMPRESS" = 1 ] && write_cmd_and_cookie "compress on" 0 || write_cmd_and_cookie "compress off" 0
stop_device; sleep 3

auc=0; catchup=-1; cycle=0
while [ "$cycle" -lt "$MAX_CYCLES" ]; do
    cycle=$((cycle+1))
    log ""
    log "── cycle $cycle ── (power on; cut in ${CYCLE_SECS}s)"
    start_device
    # Background feeder dribbles remaining flights DURING this window (transfer phase),
    # while the device harvests+uploads. Both run concurrently until the cut.
    touch "$FEEDON"; run_feeder & FEEDER=$!
    sleep "$CYCLE_SECS"
    # CUT POWER mid-stream — lands on a live transfer OR a live upload.
    stop_device
    rm -f "$FEEDON"; kill "$FEEDER" 2>/dev/null; wait "$FEEDER" 2>/dev/null
    sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null   # clear any stale feeder mount

    hwm=$(get_manifest_hwm "$SERIAL"); hwm=${hwm:-0}
    fed=$(( $(cat "$CURSOR" 2>/dev/null || echo 1) - 1 ))   # flights transferred to device so far
    bf=$((fed - hwm)); [ "$bf" -lt 0 ] && bf=0              # written-but-not-yet-uploaded (in-flight backlog)
    mb=$(s3_uploaded_mb)
    auc=$((auc + bf))
    echo "$cycle,$hwm,$bf,$mb" >> "$CSV"
    log "  RESULT cycle=$cycle fed=$fed/$NFLIGHTS hwm=$hwm inflight=${bf} s3=${mb}MB"
    if [ "$fed" -ge "$NFLIGHTS" ] && [ "$hwm" -ge "$NFLIGHTS" ] && [ "$catchup" -lt 0 ]; then
        catchup=$cycle; log "  ★ CAUGHT UP at cycle $cycle (all fed + all uploaded)"; break
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

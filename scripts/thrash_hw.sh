#!/bin/bash
# AirBridge power-cut THRASH / chaos soak — REAL HARDWARE (DEVICE ONLY).
#
# The on-hardware analogue of thrash_test.sh: seed a DSU backlog (host_dsu.py writes
# flights onto the device's real USB volume), then cut power via the CoolGear hub at
# RANDOM uptimes (default 1s..5min) and verify the unit never wedges and eventually
# gets every flight up to S3 over REAL cellular.
#
# DIFFERENCES FROM THE EMULATOR (by design):
#   • Real cellular (SIM7600/Hologram) — no scripted signal profile; whatever the
#     network gives. So this validates the pipeline + resume under real power cuts,
#     not signal-loss resilience (the emulator covers that).
#   • Real 90s USB-presentation + real PPP connect (~30-60s) per boot ⇒ a big chunk
#     of every short window is "dead" before any upload flows. Cuts shorter than that
#     are pure boot-thrash cycles (does an interrupted boot wedge it?).
#   • DSU = host_dsu.py mounting /dev/sdX1 (interleaved feeder, cursor-resumed).
#   • Monitoring via the persistent CDC serial tap ($E2E_LOG). REQUIRES the device in
#     CDC_PERSIST + an -DALLOW_CDC_PERSIST (esp32s3-e2e) build, else CDC reverts to
#     MSC-only after the first cut and boot-hang detection goes blind (hwm-only).
#
# COST: only compressed upload bytes are billed (~$0.03/MB). 200MB backlog, compress
# on ⇒ ~67MB ⇒ ~$2 (+ some re-upload churn from cuts). --no-compress ⇒ ~$6.
#
# STUCK DETECTION (any ⇒ FAIL):
#   1. NO PROGRESS — N consecutive PRODUCTIVE windows (≥ PRODUCTIVE_MIN, long enough for
#      90s USB-hold + modem resync + one flight) that had pending work but advanced the
#      manifest hwm by zero. Short/unlucky cuts don't count, so this won't false-trip.
#   2. BOOT HANG   — N consecutive long windows (≥ LIVE_BUDGET) with ZERO signs of life
#      (no fresh serial output AND no flight fed) ⇒ the unit isn't coming back. NB: the
#      boot banner is NOT used — it prints during the post-cut CDC-down window before the
#      serial tap re-enumerates, so it's systematically missed (that faked a wedge once).
#   Anomaly markers (WATCHDOG / SAFE MODE / PANIC) scanned per cycle & surfaced — a
#   pure power-cut soak should never trip the crash-loop firewall (POWERON reset).
#
# Usage (device + CoolGear hub attached):
#   scripts/thrash_hw.sh --backlog-mb 16  --flight-kb 2048            # cheap HW smoke (~$0.15)
#   scripts/thrash_hw.sh --backlog-mb 200 --min-cut 1 --max-cut 300   # full soak (hours, ~$2)
set -u

TARGET="device"
source "$(dirname "$0")/e2e_lib.sh"

# ── Params ───────────────────────────────────────────────────────────────────
SEED="${SEED:-1}"
BACKLOG_MB="${BACKLOG_MB:-200}"
FLIGHT_KB="${FLIGHT_KB:-8192}"
MIN_CUT="${MIN_CUT:-1}"
MAX_CUT="${MAX_CUT:-300}"
OFF_S="${OFF_S:-3}"                   # power-off duration between cuts (s)
FEED_GAP="${FEED_GAP:-15}"            # pause between DSU flight writes (device harvest window)
PRESEED="${PRESEED:-1}"               # 1 = seed whole backlog up front, no host writes during thrash
                                      #     (robust); 0 = interleaved feeder (fragile under aggressive cuts)
COMPRESS=1                            # on-device gzip (default on for cost)
MAX_CYCLES="${MAX_CYCLES:-200}"
MAX_WALL_MIN="${MAX_WALL_MIN:-600}"   # wall-clock cap (10h)
PRODUCTIVE_MIN="${PRODUCTIVE_MIN:-210}" # a window ≥ this is long enough (90s USB-hold + modem resync
                                        # + one flight) to expect progress; shorter cuts don't count
NOPROG_STRIKES="${NOPROG_STRIKES:-6}"   # consecutive PRODUCTIVE windows w/ pending work & no hwm advance ⇒ stuck
LIVE_BUDGET="${LIVE_BUDGET:-160}"       # a window ≥ this with ZERO signs of life (no serial, no feed) ⇒ hang strike
HANG_STRIKES="${HANG_STRIKES:-4}"       # consecutive dead long windows ⇒ boot hang
SERIAL="${SERIAL:-EA500.E2ETES}"

while [ $# -gt 0 ]; do
    case "$1" in
        --seed)         SEED="$2"; shift 2 ;;
        --backlog-mb)   BACKLOG_MB="$2"; shift 2 ;;
        --flight-kb)    FLIGHT_KB="$2"; shift 2 ;;
        --min-cut)      MIN_CUT="$2"; shift 2 ;;
        --max-cut)      MAX_CUT="$2"; shift 2 ;;
        --off)          OFF_S="$2"; shift 2 ;;
        --feed-gap)     FEED_GAP="$2"; shift 2 ;;
        --no-compress)  COMPRESS=0; shift ;;
        --feeder)       PRESEED=0; shift ;;   # use interleaved feeder instead of pre-seed
        --max-cycles)   MAX_CYCLES="$2"; shift 2 ;;
        --max-wall-min) MAX_WALL_MIN="$2"; shift 2 ;;
        --productive-min) PRODUCTIVE_MIN="$2"; shift 2 ;;
        --noprog-strikes) NOPROG_STRIKES="$2"; shift 2 ;;
        *) echo "unknown arg: $1"; exit 1 ;;
    esac
done

NFLIGHTS=$(( (BACKLOG_MB * 1024 + FLIGHT_KB - 1) / FLIGHT_KB ))
PASS=0; FAIL=0; SKIP=0
RANDOM="$SEED"
HOSTDSU="python3 $HOME/USBCellular/scripts/host_dsu.py"
CSV="/tmp/thrash_hw_seed${SEED}.csv"
STUCKLOG="/tmp/thrash_hw_seed${SEED}_stuck.log"
CURSOR=/tmp/thrash_hw_next_flight
FEEDON=/tmp/thrash_hw_feed_on

# ── DSU feeder (from flight_cycle_hw.sh) ──────────────────────────────────────
feed_one_flight() {   # $1 = flight number; 0=written 1=device gone 2=transient
    local f="$1" dev d w
    dev=""
    for w in $(seq 1 8); do
        for d in /dev/sda1 /dev/sdb1 /dev/sdc1 /dev/sdd1; do [ -b "$d" ] && { dev="$d"; break; }; done
        [ -n "$dev" ] && break
        sleep 1
    done
    [ -z "$dev" ] && return 1
    if ! sudo mount -o noatime,uid=$(id -u),gid=$(id -g) "$dev" /mnt 2>/dev/null; then
        # Mount failed — the aggressive cuts likely left the FAT dirty/inconsistent.
        # Repair it (host-side) and retry once; the device's FATFS tolerates dirt but
        # Linux vfat does not, which otherwise locks the feeder out permanently.
        sudo fsck.vfat -a "$dev" >/dev/null 2>&1
        sudo mount -o noatime,uid=$(id -u),gid=$(id -g) "$dev" /mnt 2>/dev/null || return 2
    fi
    if ! $HOSTDSU --mount /mnt --serial "$SERIAL" --max-flight "$f" --size-kb "$FLIGHT_KB" \
                  --compressible --ignore-cookie >/dev/null 2>&1; then
        sync 2>/dev/null; sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null
        return 2
    fi
    sync 2>/dev/null; sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null
    return 0
}
run_feeder() {   # background: keep feeding remaining flights so cuts land mid-work
    local misses=0
    while [ -f "$FEEDON" ]; do
        local f; f=$(cat "$CURSOR" 2>/dev/null || echo 1)
        [ "$f" -gt "$NFLIGHTS" ] && break
        feed_one_flight "$f"; local rc=$?
        if [ "$rc" = 0 ]; then
            echo $((f+1)) > "$CURSOR"; log "  fed flight $f/$NFLIGHTS"; misses=0
            local g=0; while [ -f "$FEEDON" ] && [ "$g" -lt "$FEED_GAP" ]; do sleep 1; g=$((g+1)); done
        elif [ "$rc" = 1 ]; then
            misses=$((misses+1)); [ "$misses" -ge 20 ] && break
            sleep 1
        else
            sleep 3
        fi
    done
}

find_usb_part() { local w d; for w in $(seq 1 150); do for d in /dev/sda1 /dev/sdb1 /dev/sdc1 /dev/sdd1; do [ -b "$d" ] && { echo "$d"; return 0; }; done; sleep 1; done; return 1; }

# PRE-SEED: write the ENTIRE backlog (NFLIGHTS distinct single-flight files) onto
# flightHistory ONCE, up front, while the FAT is clean and the host isn't racing power
# cuts. Then the thrash loop does NO host writes — it just cuts power while the device
# drains what's already on the SD. This is the emulator's model and avoids the dirty-FAT
# / MSC-contention failures that plague interleaved feeding under aggressive cuts.
# Each flight f is written first=f last=f (a distinct flight) so hwm advances 1..N in order.
# Robust to intermittent MSC I/O: retries per flight, syncs each, remounts on error.
seed_backlog() {
    local dev seeded=0 f a mounted=0
    dev=$(find_usb_part) || { log "  SEED: no USB partition"; return 1; }
    # ONE continuous mount for the whole backlog. Writing back-to-back gives the device
    # no 15s-quiet window, so it won't harvest (and hold the SD) mid-seed — that's what
    # broke the per-flight-remount approach. Remount ONLY if a write actually errors.
    sudo umount /mnt 2>/dev/null
    _seed_mount() { sudo mount -o rw,noatime,flush,uid=$(id -u),gid=$(id -g) "$dev" /mnt 2>/dev/null; }
    _seed_mount && mounted=1
    for f in $(seq 1 "$NFLIGHTS"); do
        local wrote=0
        for a in $(seq 1 6); do
            [ "$mounted" = 1 ] || { _seed_mount && mounted=1 || { sleep 2; continue; }; }
            if $HOSTDSU --mount /mnt --serial "$SERIAL" --max-flight "$f" --first-flight "$f" \
                        --size-kb "$FLIGHT_KB" --compressible --ignore-cookie >/dev/null 2>&1; then
                wrote=1; break
            fi
            # write failed — flush, drop the mount, let USB settle, remount fresh
            sync 2>/dev/null; sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null
            mounted=0; sleep 3
        done
        if [ "$wrote" = 1 ]; then seeded=$((seeded+1)); else log "  SEED: flight $f FAILED after retries"; fi
        [ $((f % 5)) -eq 0 ] && log "  SEED: $seeded/$NFLIGHTS flights written"
    done
    [ "$mounted" = 1 ] && { sync 2>/dev/null; sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null; }
    log "  SEED: done — $seeded/$NFLIGHTS flights on flightHistory"
    echo "$((NFLIGHTS+1))" > "$CURSOR"   # feeder is disabled in preseed; mark backlog fully 'fed'
    [ "$seeded" -ge "$NFLIGHTS" ]
}

write_cmd_and_cookie() {   # $1 directive  $2 cookie-flight
    local directive="$1" cookie="${2:-}"
    local dev; dev=$(find_usb_part) || { log "  no USB partition to write cmd"; return 1; }
    sudo mount -o noatime,uid=$(id -u),gid=$(id -g) "$dev" /mnt 2>/dev/null || { log "  mount failed"; return 1; }
    printf '%s\n' "$directive" | sudo tee /mnt/airbridge.cmd >/dev/null
    [ -n "$cookie" ] && $HOSTDSU --mount /mnt --serial "$SERIAL" --plant-cookie "$cookie" >/dev/null
    sync; sudo umount /mnt 2>/dev/null
    log "  airbridge.cmd='$directive'${cookie:+ cookie=$cookie}"
}

s3_uploaded_mb() { aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null | awk '{s+=$3} END{printf "%.1f", s/1048576}'; }

anomaly_scan() {  # after_line
    tail -n "+$(( ${1:-0} + 1 ))" "$E2E_LOG" 2>/dev/null \
        | grep -Eio "WATCHDOG[^ ]*|SAFE MODE|PANIC|abort\(\)|Guru Meditation|assert failed" \
        | sort -u | tr '\n' ' '
}

# Lightweight power-on that RETURNS IMMEDIATELY (unlike e2e_lib start_device, which
# blocks up to 130s on presentation and would eat the random window). The window is
# measured from power-on; the feeder waits for USB on its own.
power_on() { $COOLGEAR on >/dev/null 2>&1; serial_tap_start; }

# ── Setup ─────────────────────────────────────────────────────────────────────
command -v aws >/dev/null || { echo "aws CLI required"; exit 1; }
log "═══════════════════════════════════════════════════════════════"
log "  POWER-CUT THRASH — HARDWARE  serial=$SERIAL seed=$SEED"
log "  backlog≈${BACKLOG_MB}MB ($NFLIGHTS flights @${FLIGHT_KB}KB) compress=$COMPRESS"
log "  cuts=random(${MIN_CUT}..${MAX_CUT})s  productive_min=${PRODUCTIVE_MIN}s noprog_strikes=${NOPROG_STRIKES}  max_wall=${MAX_WALL_MIN}min"
log "═══════════════════════════════════════════════════════════════"

cleanup_aircraft_s3 "$SERIAL"
echo "cycle,window_s,uptime_s,hwm,fed,nflights,inflight,progress,alive,anomalies" > "$CSV"

# Setup boot: stage compress directive + fresh cookie, ensure CDC persists, (pre-seed), then off.
log "── setup: compress=$COMPRESS + reset cookie 0 + CDC_PERSIST${PRESEED:+ + preseed}${PRESEED:+ backlog} ──"
echo 1 > "$CURSOR"; rm -f "$FEEDON"
power_on
[ "$COMPRESS" = 1 ] && write_cmd_and_cookie "compress on" 0 || write_cmd_and_cookie "compress off" 0
device_enable_persistent_cdc
if [ "$PRESEED" = 1 ]; then
    log "── seeding full ${BACKLOG_MB}MB backlog onto flightHistory (USB presented) ──"
    seed_backlog || log "  WARN: seed incomplete — thrash will drain whatever landed"
fi
$COOLGEAR off >/dev/null 2>&1; sleep 3

# ── Thrash loop ────────────────────────────────────────────────────────────────
prev_hwm=0; noprog_strikes=0; hang_strikes=0
cycle=0; caught_up=0; stuck=0; boot_hang=0
start_wall=$(date +%s)
max_wall_s=$((MAX_WALL_MIN*60))

while :; do
    now=$(date +%s); elapsed=$((now-start_wall))
    hwm_now=$(get_manifest_hwm "$SERIAL"); hwm_now=${hwm_now:-0}
    fed_now=$(( $(cat "$CURSOR" 2>/dev/null || echo 1) - 1 ))
    if [ "$hwm_now" -ge "$NFLIGHTS" ] && [ "$fed_now" -ge "$NFLIGHTS" ]; then caught_up=1; break; fi
    [ "$cycle" -ge "$MAX_CYCLES" ] && { log "  reached --max-cycles=$MAX_CYCLES"; break; }
    [ "$elapsed" -ge "$max_wall_s" ] && { log "  reached --max-wall-min=$MAX_WALL_MIN"; break; }

    cycle=$((cycle+1))
    span=$((MAX_CUT - MIN_CUT + 1)); [ "$span" -lt 1 ] && span=1
    window=$((MIN_CUT + RANDOM % span))
    mark=$(log_mark)               # serial tap is persistent (NOT truncated) — mark works
    fed_start=$(( $(cat "$CURSOR" 2>/dev/null || echo 1) - 1 ))

    power_on
    t0=$(date +%s)
    FEEDER=""
    if [ "$PRESEED" != 1 ]; then    # interleaved feeder (only when not pre-seeded)
        touch "$FEEDON"; run_feeder & FEEDER=$!
    fi
    # Run the window (device harvests/uploads the backlog until the cut).
    while :; do el=$(( $(date +%s) - t0 )); [ "$el" -ge "$window" ] && break; sleep 1; done
    uptime_s=$(( $(date +%s) - t0 ))

    # CUT POWER mid-stream.
    rm -f "$FEEDON"; [ -n "$FEEDER" ] && { kill "$FEEDER" 2>/dev/null; wait "$FEEDER" 2>/dev/null; }
    power_cut "$OFF_S"
    sudo umount /mnt 2>/dev/null || sudo umount -l /mnt 2>/dev/null

    hwm=$(get_manifest_hwm "$SERIAL"); hwm=${hwm:-0}
    fed=$(( $(cat "$CURSOR" 2>/dev/null || echo 1) - 1 ))
    inflight=$((fed - hwm)); [ "$inflight" -lt 0 ] && inflight=0
    anom=$(anomaly_scan "$mark")
    # LIVENESS (robust): the unit came back this cycle if it produced ANY fresh serial
    # output OR fed a flight (both require a booted, USB-presenting device). The boot
    # banner itself is unreliable — it prints during the post-cut CDC-down window before
    # the serial tap re-enumerates, so it's systematically missed.
    new_serial=$(tail -n "+$((mark+1))" "$E2E_LOG" 2>/dev/null | wc -l | tr -d ' ')
    fed_this=$((fed - fed_start)); [ "$fed_this" -lt 0 ] && fed_this=0
    alive=0; { [ "$new_serial" -gt 0 ] || [ "$fed_this" -gt 0 ]; } && alive=1
    productive=0; [ "$window" -ge "$PRODUCTIVE_MIN" ] && productive=1
    progress=0
    if [ "$hwm" -gt "$prev_hwm" ]; then
        progress=$((hwm - prev_hwm)); prev_hwm=$hwm; noprog_strikes=0
    elif [ "$productive" = 1 ] && [ "$inflight" -gt 0 ]; then
        noprog_strikes=$((noprog_strikes+1))   # a long window w/ pending work made no progress
    fi
    # Hang strikes: a long-enough window with ZERO signs of life.
    if [ "$window" -ge "$LIVE_BUDGET" ] && [ "$alive" = 0 ]; then
        hang_strikes=$((hang_strikes+1))
    else
        [ "$alive" = 1 ] && hang_strikes=0
    fi
    echo "$cycle,$window,$uptime_s,$hwm,$fed,$NFLIGHTS,$inflight,$progress,$alive,${anom// /|}" >> "$CSV"
    mb=$(s3_uploaded_mb)
    log "  cycle $cycle: window=${window}s hwm=$hwm/$NFLIGHTS fed=$fed inflight=$inflight progress=+$progress alive=$alive prod=$productive noprog_strk=$noprog_strikes s3=${mb}MB${anom:+  ANOMALY: $anom}"

    if [ "$noprog_strikes" -ge "$NOPROG_STRIKES" ]; then
        stuck=1; log "  ★ STUCK: $noprog_strikes productive windows w/ pending work and NO hwm advance (cycle $cycle)"; break
    fi
    if [ "$hang_strikes" -ge "$HANG_STRIKES" ]; then
        stuck=1; boot_hang=1; log "  ★ STUCK: $hang_strikes long windows with zero signs of life — unit not rebooting"; break
    fi
done

# ── Verdict ────────────────────────────────────────────────────────────────────
$COOLGEAR off >/dev/null 2>&1; rm -f "$FEEDON"
serial_tap_stop
final_hwm=$(get_manifest_hwm "$SERIAL"); final_hwm=${final_hwm:-0}
s3_objs=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null | grep -c '\.eaofh')
final_mb=$(s3_uploaded_mb); cost=$(awk -v m="$final_mb" 'BEGIN{printf "%.2f", m*0.03}')
wall=$(( ($(date +%s) - start_wall) / 60 ))
log ""
log "═══════════════════════════════════════════════════════════════"
log "  HARDWARE THRASH RESULT  seed=$SEED"
log "    cycles=$cycle  wall≈${wall}min"
log "    final hwm=$final_hwm / $NFLIGHTS   S3 .eaofh objects=$s3_objs   billed≈${final_mb}MB (\$$cost)"
log "    CSV: $CSV"
if [ "$stuck" = 1 ]; then
    cp -f "$E2E_LOG" "$STUCKLOG" 2>/dev/null; log "    forensic serial log: $STUCKLOG"
    if [ "$boot_hang" = 1 ]; then fail "WEDGED (boot hang) at cycle $cycle — hwm=$final_hwm/$NFLIGHTS"
    else fail "WEDGED (no progress) — hwm=$final_hwm/$NFLIGHTS after $cycle cycles"; fi
elif [ "$caught_up" = 1 ]; then
    if [ "$final_hwm" -ge "$NFLIGHTS" ] && [ "$s3_objs" -ge "$NFLIGHTS" ]; then
        pass "Drained all $NFLIGHTS flights to S3 under HW power-cut thrash ($cycle cycles, ~${wall}min, \$$cost)"
    else fail "hwm caught up ($final_hwm) but S3 has only $s3_objs/$NFLIGHTS .eaofh objects"; fi
else fail "Did not catch up (hwm=$final_hwm/$NFLIGHTS) within limits"; fi
log "═══════════════════════════════════════════════════════════════"
exit "$FAIL"

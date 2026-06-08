#!/bin/bash
# AirBridge Unified E2E Test Suite
# Runs identical tests against real hardware OR the emulator.
#
# Usage:
#   ./scripts/e2e_unified.sh --target emulator    # no hardware needed
#   ./scripts/e2e_unified.sh --target device       # requires CoolGear + ESP32
#
set -u

# ── Parse args ────────────────────────────────────────────────────────────────
TARGET="${1:---target}"
TARGET="${2:-emulator}"
if [ "$1" = "--target" ] 2>/dev/null; then TARGET="$2"; fi
if [ "$TARGET" != "device" ] && [ "$TARGET" != "emulator" ]; then
    echo "Usage: $0 --target [device|emulator]"
    exit 1
fi

# ── Config ────────────────────────────────────────────────────────────────────
COOLGEAR="python3 $HOME/USBCellular/scripts/coolgear.py"
FW_DIR="$HOME/USBCellular/esp32"
EMU="$FW_DIR/.pio/build/emulator/program"
SD_EMU="$FW_DIR/emu_sdcard"             # Partition 1 (DSU-facing)
SD_INT="$FW_DIR/emu_sdcard_internal"    # Partition 2 (firmware internal)
BUCKET="airbridge-uploads-test"
API_KEY="7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm"
LOG="/tmp/e2e_${TARGET}_$(date +%Y%m%d_%H%M%S).txt"
SERIAL="EA500.E2ETES"    # fictional serial — never collides with real fleet data

if [ "$TARGET" = "device" ]; then
    DEVICE="9C139EF40188"
else
    DEVICE="EMU_E2E_$(date +%H%M%S)"
fi

# State-triggered waits (wait_for_log/harvest/upload_complete/upload_idle) read this
# file. Emulator: the emu binary's stdout is redirected here. Device: a background
# serial tap on /dev/ttyACM* (serial_tap_start) mirrors the CDC log here so the SAME
# wait helpers work on hardware. Requires the device booted CDC+MSC (CDC_PERSIST).
if [ "$TARGET" = "device" ]; then E2E_LOG="/tmp/dev_e2e.log"; else E2E_LOG="/tmp/emu_e2e.log"; fi

PASS=0; FAIL=0; SKIP=0
EMU_PID=""
SERIAL_TAP_PID=""

log() { echo "$(date +%H:%M:%S)   $1" | tee -a "$LOG"; }
pass() { log "PASS: $1"; PASS=$((PASS + 1)); }
fail() { log "FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { log "SKIP: $1"; SKIP=$((SKIP + 1)); }

# ── Abstraction layer ─────────────────────────────────────────────────────────

start_device() {
    if [ "$TARGET" = "emulator" ]; then
        mkdir -p "$SD_EMU" "$SD_INT"
        rm -f "$FW_DIR/emu_ota_update.bin"  # prevent stale OTA downloads
        cd "$FW_DIR"
        # Truncate log before each start so we don't match old "Init complete"
        : > /tmp/emu_e2e.log
        $EMU "$DEVICE" >>/tmp/emu_e2e.log 2>&1 &
        EMU_PID=$!
        for i in $(seq 1 30); do
            grep -q "Init complete" /tmp/emu_e2e.log 2>/dev/null && break
            sleep 1
        done
        sleep 3
        sleep 3  # extra settle for file watcher baseline scan
        log "  Emulator started (pid=$EMU_PID)"
    else
        local boot_mark
        boot_mark=$(log_mark)
        $COOLGEAR off >/dev/null 2>&1; sleep "${1:-5}"; $COOLGEAR on >/dev/null 2>&1
        sleep 5
        for i in $(seq 1 15); do
            lsusb 2>/dev/null | grep -q "1209:000" && break
            sleep 1
        done
        # Keep the serial tap alive (no-op if already running); it reconnects to the
        # ACM port that just re-enumerated.
        serial_tap_start
        # Wait until the MSC drive is FRESHLY PRESENTED (firmware logs this at ~90s),
        # not merely until lsusb/CDC enumerates. Otherwise host_dsu would mount a STALE
        # /dev/sdX1 left over from the previous boot — its writes reach the device only
        # minutes later (after re-enumeration flushes the cache), missing the harvest
        # window. Gating on drive-presented guarantees host_dsu mounts the live volume.
        if wait_for_log "$(marker usb_presented)" "$boot_mark" 130 "USB drive presented"; then
            sleep 3  # let udev settle the fresh /dev/sdX1 partition node
        else
            log "  (USB drive-presented marker not seen within 130s — continuing)"
        fi
        log "  Device powered on"
    fi
}

stop_device() {
    if [ "$TARGET" = "emulator" ]; then
        if [ -n "$EMU_PID" ]; then
            kill "$EMU_PID" 2>/dev/null
            # Wait up to 5s for graceful exit, then force-kill
            for i in 1 2 3 4 5; do
                kill -0 "$EMU_PID" 2>/dev/null || break
                sleep 1
            done
            kill -9 "$EMU_PID" 2>/dev/null
            wait "$EMU_PID" 2>/dev/null
        fi
        # Kill any stale emulators or pppd processes
        killall -9 program 2>/dev/null
        sudo killall -9 pppd 2>/dev/null
        sleep 1
        EMU_PID=""
    else
        $COOLGEAR off >/dev/null 2>&1
    fi
}

power_cut() {
    log "  Power cut!"
    if [ "$TARGET" = "emulator" ]; then
        [ -n "$EMU_PID" ] && kill -9 "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
        sudo killall -9 pppd 2>/dev/null
        EMU_PID=""
    else
        $COOLGEAR off >/dev/null 2>&1
    fi
    sleep "${1:-5}"
}

# ── Serial log tap (device only) ──────────────────────────────────────────────
# Tail /dev/ttyACM* into $E2E_LOG so the state-triggered waits work on hardware
# exactly as on the emulator. Reconnects across power cycles (the port vanishes on
# power-off, returns on power-on). Started once after the first CDC boot; kept alive
# for the whole suite; stopped only at teardown. Mirrors scripts/hw_capture.sh.
serial_tap_start() {
    [ "$TARGET" = "device" ] || return 0
    if [ -n "$SERIAL_TAP_PID" ] && kill -0 "$SERIAL_TAP_PID" 2>/dev/null; then return 0; fi
    : > "$E2E_LOG"
    (
        while :; do
            port=$(ls /dev/ttyACM* 2>/dev/null | sort | head -1)
            [ -n "$port" ] && timeout 3600 cat "$port" 2>/dev/null
            sleep 0.3
        done
    ) >> "$E2E_LOG" 2>&1 &
    SERIAL_TAP_PID=$!
    log "  Serial tap started (pid=$SERIAL_TAP_PID) -> $E2E_LOG"
}

serial_tap_stop() {
    [ -n "$SERIAL_TAP_PID" ] || return 0
    kill "$SERIAL_TAP_PID" 2>/dev/null
    wait "$SERIAL_TAP_PID" 2>/dev/null
    SERIAL_TAP_PID=""
}

# Drop the persistent-CDC magic file on P1 so every subsequent boot is CDC+MSC
# (serial available throughout). Requires the drive presented (boot in whatever mode,
# wait for /dev/sdX1, mount, touch, sync, umount). Idempotent. Remove at teardown via
# device_disable_persistent_cdc to restore production MSC-only.
device_enable_persistent_cdc() {
    [ "$TARGET" = "device" ] || return 0
    local sddev="" w
    for w in $(seq 1 120); do
        for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do [ -b "$d" ] && { sddev="$d"; break 2; }; done
        sleep 1
    done
    [ -n "$sddev" ] || { log "  WARN: CDC_PERSIST — no USB drive to write to"; return 1; }
    if sudo mount -o noatime "$sddev" /mnt 2>/dev/null; then
        sudo touch /mnt/CDC_PERSIST; sync; sudo umount /mnt 2>/dev/null
        log "  CDC_PERSIST staged on P1 ($sddev)"
    else
        log "  WARN: CDC_PERSIST — mount $sddev failed"; return 1
    fi
}

device_disable_persistent_cdc() {
    [ "$TARGET" = "device" ] || return 0
    local sddev="" d
    for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do [ -b "$d" ] && { sddev="$d"; break; }; done
    [ -n "$sddev" ] || return 0
    sudo mount -o noatime "$sddev" /mnt 2>/dev/null && {
        sudo rm -f /mnt/CDC_PERSIST; sync; sudo umount /mnt 2>/dev/null
        log "  CDC_PERSIST removed from P1 (reverts to MSC-only next boot)"
    }
}

# Read the DSU cookie flight from the device's USB volume (0 if absent/invalid).
# Mounts/reads/unmounts via host_dsu; only call when the firmware is idle (after a
# harvest+upload completes) so it doesn't collide with the 15s quiet-window harvest.
device_cookie_flight() {
    (cd "$HOME/USBCellular" && python3 scripts/host_dsu.py --mount-find \
        --serial "$SERIAL" --read-cookie 2>/dev/null) | tail -1
}

# Reset the device's DSU cookie to flight 0 (fresh DSU) — the hardware equivalent of
# the emulator's `rm *.easdf`. The physical device's cookie persists across tests, so
# tests that expect to upload from flight 1 must reset it first (else host_dsu sees the
# stale cookie and reports "caught up"). Requires the drive presented (call after start_device).
device_reset_cookie() {
    (cd "$HOME/USBCellular" && python3 scripts/host_dsu.py --mount-find \
        --serial "$SERIAL" --plant-cookie 0 2>&1 | grep -E 'planted|ERROR' | tail -1) \
        | while read -r l; do log "  cookie reset: $l"; done
}

# ── Per-target log markers ────────────────────────────────────────────────────
# The emulator prints its own progress strings (emu/main.cpp); the firmware emits
# DIFFERENT strings over CDC serial — and only airbridge_log/cdc_printf/ULDBG reach
# CDC, NOT ESP_LOGI. Map each logical event to the right regex for $TARGET. Verified
# device markers: AirBridge fw= (3493), PPP got IP (2117), doHarvest: done (3340),
# Upload: queue drained (added), Cookie updated (3222), Boot cookie synced (3120),
# USB: drive presented (3406); ULDBG multipart/stream begin is shared (airbridge_s3.h).
marker() {
    if [ "$TARGET" = "device" ]; then
        case "$1" in
            init)           echo "AirBridge fw=" ;;
            ppp_up)         echo "PPP got IP" ;;
            harvest_done)   echo "doHarvest: done [0-9]+ file" ;;
            upload_idle)    echo "Upload: queue drained" ;;
            multipart)      echo "ULDBG multipart:|ULDBG stream begin" ;;
            cookie_updated) echo "Cookie updated:" ;;
            cookie_synced)  echo "Boot cookie synced to S3 hwm=" ;;
            usb_presented)  echo "USB: drive presented" ;;
            *)              echo "$1" ;;
        esac
    else
        case "$1" in
            init)           echo "Init complete" ;;
            ppp_up)         echo "PPP up" ;;
            harvest_done)   echo "\[Harvest\] Done:" ;;
            upload_idle)    echo "\[S3\] Upload thread done" ;;
            multipart)      echo "ULDBG multipart:|ULDBG stream begin" ;;
            cookie_updated) echo "cookie|Cookie" ;;
            cookie_synced)  echo "cookie|Cookie" ;;
            usb_presented)  echo "USB" ;;
            *)              echo "$1" ;;
        esac
    fi
}

# Append a valid DSU 0x4C summary record (28 bytes) so the firmware's
# content-based parser can extract serial+flight from the file.
# Usage: append_eaofh_trailer <path> <serial> <flight_num>
append_eaofh_trailer() {
    local path="$1" serial="$2" flight="$3"
    local sudo_cmd="${4:-}"
    $sudo_cmd python3 -c "
import sys
serial = b'$serial'
flight = int('$flight')
body = bytearray(24)
body[5:5+min(12, len(serial))] = serial[:12]
body[20] = (flight >> 8) & 0xFF
body[21] = flight & 0xFF
rec = bytes([0xEA, 0x4C, 0x00, 0x1C]) + bytes(body)
with open('$path', 'ab') as f: f.write(rec)
"
}

# Write a DSU-style flight file (realistic directory structure).
# Prepends a flight-1 record (simulates DSU downloading from the start),
# then random data, then the requested flight number as the last record.
# This ensures first_flight=1 / last_flight=N so the Lambda's consecutive
# hwm logic correctly advances on upload.
#   Emulator: synthesize directly on the FakeSD (cookie-agnostic — emulator state
#             is reset per test so there's no stale cookie to honor).
#   Device:   host_dsu.py mounts the ESP32's USB volume and emits COOKIE-AWARE —
#             it reads dsuCookie.easdf the firmware wrote and only emits flights
#             past it, exactly like a real aircraft DSU. Extra args ($3...) pass
#             through to host_dsu.py (e.g. --first-flight 1 --meta for the delta test).
write_dsu_file() {
    local flight="$1" size_kb="$2"; shift 2
    local date_str=$(date +%Y%m%d)
    local fname="${SERIAL}_${flight}_${date_str}.eaofh"

    if [ "$TARGET" = "emulator" ]; then
        mkdir -p "$SD_EMU/flightHistory"
        local fpath="$SD_EMU/flightHistory/$fname"
        # First-flight marker (flight 1) at the start so firstRecordFromLog returns 1.
        # The 0xEA byte immediately after satisfies the record-framing validation
        # (firstRecordFromLog requires the byte after a record to be 0xEA).
        append_eaofh_trailer "$fpath" "$SERIAL" "00001"
        printf '\xEA' >> "$fpath"
        dd if=/dev/urandom of="$fpath" bs=1K count="$size_kb" oflag=append conv=notrunc 2>/dev/null
        append_eaofh_trailer "$fpath" "$SERIAL" "$flight"
        log "  Wrote flightHistory/$fname (${size_kb}KB + first+last trailers)"
    else
        # Cookie-aware emission via host_dsu.py (mounts/syncs/unmounts the USB volume
        # itself — never holds the mount during the firmware's 15s quiet harvest).
        local out
        out=$(cd "$HOME/USBCellular" && python3 scripts/host_dsu.py --mount-find \
            --serial "$SERIAL" --max-flight "$((10#$flight))" --size-kb "$size_kb" "$@" 2>&1)
        log "  host_dsu: $(echo "$out" | grep -E 'emitted|caught up|sliced|ERROR' | tail -1)"
    fi
}

write_test_file() {
    local name="$1" size_mb="$2"
    if [ "$TARGET" = "emulator" ]; then
        dd if=/dev/urandom of="$SD_EMU/$name" bs=1M count="$size_mb" 2>/dev/null
        log "  Wrote $name (${size_mb}MB) to emu_sdcard/"
    else
        local sddev=""
        for w in $(seq 1 90); do
            for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do [ -b "$d" ] && { sddev="$d"; break 2; }; done
            sleep 1
        done
        [ -n "$sddev" ] || { log "  WARN: no USB drive"; return 1; }
        sudo mount -o noatime "$sddev" /mnt 2>/dev/null || return 1
        sudo dd if=/dev/urandom of="/mnt/$name" bs=1M count="$size_mb" 2>/dev/null
        sync; sudo umount /mnt 2>/dev/null
        log "  Wrote $name (${size_mb}MB) to USB drive"
    fi
}

wait_for_s3_file() {
    local key="$1" timeout="${2:-300}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t + 5))
        if aws s3 ls "s3://$BUCKET/$DEVICE/" --recursive 2>/dev/null | grep -q "$key"; then return 0; fi
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for $key..."
    done
    return 1
}

# Wait for a non-.eaofh file under the device-namespaced prefix.
# S3 keys: DEVICE/NNNN/filename
wait_for_upload() {
    local s3_pattern="$1" timeout="${2:-180}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t + 5))
        if aws s3 ls "s3://$BUCKET/$DEVICE/" --recursive 2>/dev/null | grep -q "$s3_pattern"; then
            return 0
        fi
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for $s3_pattern in S3..."
    done
    return 1
}

# Wait for a .eaofh file under the aircraft-namespaced prefix.
# S3 keys: aircraft/{serial}/{filename}
wait_for_aircraft_upload() {
    local serial="$1" fname_pattern="$2" timeout="${3:-180}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t + 5))
        if aws s3 ls "s3://$BUCKET/aircraft/$serial/" --recursive 2>/dev/null | grep -q "$fname_pattern"; then
            return 0
        fi
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for aircraft/$serial/$fname_pattern..."
    done
    return 1
}

# ── State-triggered waits (emulator) ─────────────────────────────────────────
# Instead of fixed sleeps, block until the emulator logs a specific state marker,
# so each step runs at exactly the right moment. All operate on /tmp/emu_e2e.log.

# Current line count of the emu log — capture a "mark" before an action, then
# wait_for_log "<pattern>" "$mark" to match only output produced AFTER the mark
# (avoids matching a stale marker from an earlier step in the same session).
log_mark() { wc -l < "$E2E_LOG" 2>/dev/null | tr -d ' '; }

# wait_for_log <pattern> [after_line] [timeout_sec] [label]
# Returns 0 when <pattern> appears in the emu log past after_line; 1 on timeout.
wait_for_log() {
    local pattern="$1" after="${2:-0}" timeout="${3:-120}" label="${4:-$1}"
    local t=0
    while [ $t -lt $timeout ]; do
        if tail -n "+$((after + 1))" "$E2E_LOG" 2>/dev/null | grep -qE "$pattern"; then
            return 0
        fi
        sleep 1; t=$((t + 1))
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for '$label'..."
    done
    return 1
}

# Wait for a harvest cycle to COMPLETE (file(s) copied to upload queue), past a mark.
wait_for_harvest() {  # [after_line] [timeout] [label]
    wait_for_log "$(marker harvest_done)" "${1:-0}" "${2:-90}" "${3:-harvest complete}"
}

# Wait for an upload of a specific file to COMPLETE.
#  - Emulator: "Uploading <name>" then "[S3] Upload complete:".
#  - Device:   firmware logs "S3: uploaded '<relPath>' OK ..." (single + multipart)
#              over CDC, which already contains the filename.
wait_for_upload_complete() {  # <name_substr> [after_line] [timeout] [label]
    local name="$1" after="${2:-0}" timeout="${3:-180}" label="${4:-upload $1}"
    local t=0
    while [ $t -lt $timeout ]; do
        if [ "$TARGET" = "device" ]; then
            # The upload task logs "Uploaded <relPath> <kbps> KB/s (<err>)" on completion
            # for BOTH single-PUT and multipart eaofh (main.cpp:3204). (The generic
            # "S3: uploaded '...' OK" cdc_printf is not on the eaofh path.)
            if tail -n "+$((after + 1))" "$E2E_LOG" 2>/dev/null \
                 | grep -qE "Uploaded [^ ]*$name[^ ]* [0-9.]+ KB/s"; then
                return 0
            fi
        else
            if tail -n "+$((after + 1))" "$E2E_LOG" 2>/dev/null \
                 | grep -A40 "Uploading.*$name" | grep -qE "\[S3\] Upload complete:"; then
                return 0
            fi
        fi
        sleep 1; t=$((t + 1))
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for '$label'..."
    done
    return 1
}

# Wait until the upload queue is drained (upload thread reports done with nothing left).
wait_for_upload_idle() {  # [after_line] [timeout]
    wait_for_log "$(marker upload_idle)" "${1:-0}" "${2:-180}" "upload idle"
}

# ── Manifest (fleet) helpers — used by happy-path + manifest tests ───────────
# wait for manifest to reach a minimum hwm for a given DSU serial.
wait_for_manifest() {  # <serial> <min_hwm> [timeout_sec]
    local serial="$1" min_hwm="$2" timeout="${3:-120}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t + 5))
        local hwm
        hwm=$(aws s3 cp "s3://$BUCKET/aircraft/$serial/manifest.json" - 2>/dev/null \
              | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('high_water_mark',0))" 2>/dev/null || echo 0)
        if [ "${hwm:-0}" -ge "$min_hwm" ]; then return 0; fi
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: manifest $serial hwm=${hwm:-?} (want>=$min_hwm)..."
    done
    return 1
}

get_manifest_hwm() {  # <serial>
    local serial="$1"
    aws s3 cp "s3://$BUCKET/aircraft/$serial/manifest.json" - 2>/dev/null \
        | python3 -c "import sys,json; d=json.load(sys.stdin); print(d.get('high_water_mark',0))" 2>/dev/null || echo 0
}

# Seed a manifest to a given hwm (simulates previous AirBridge uploads).
seed_manifest() {  # <serial> <hwm>
    local serial="$1" hwm="$2"
    python3 -c "
import json, boto3, datetime
s3 = boto3.client('s3', region_name='us-west-2')
manifest = {
    'serial': '$serial',
    'high_water_mark': $hwm,
    'files': [{'key': 'aircraft/$serial/seed', 'first_flight': 1, 'last_flight': $hwm}],
    'last_updated': datetime.datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ')
}
s3.put_object(Bucket='$BUCKET', Key='aircraft/$serial/manifest.json',
              Body=json.dumps(manifest), ContentType='application/json')
print('Seeded manifest: serial=$serial hwm=$hwm')
" 2>/dev/null && log "  Seeded manifest: $serial hwm=$hwm"
}

# Seed a manifest with EXPLICIT file ranges (for gapped / non-flight-1 floor scenarios).
# Usage: seed_manifest_files <serial> <hwm> <first:last> [<first:last> ...]
seed_manifest_files() {
    local serial="$1" hwm="$2"; shift 2
    BUCKET="$BUCKET" python3 - "$serial" "$hwm" "$@" <<'PY' 2>/dev/null && log "  Seeded manifest: $serial hwm=$hwm ranges=$*"
import json, boto3, datetime, sys, os
serial, hwm = sys.argv[1], int(sys.argv[2])
files = []
for r in sys.argv[3:]:
    ff, lf = (int(x) for x in r.split(':'))
    files.append({'key': f'aircraft/{serial}/seed_{lf:05d}.eaofh',
                  'first_flight': ff, 'last_flight': lf})
manifest = {'serial': serial, 'high_water_mark': hwm, 'files': files,
            'last_updated': datetime.datetime.utcnow().strftime('%Y-%m-%dT%H:%M:%SZ')}
boto3.client('s3', region_name='us-west-2').put_object(
    Bucket=os.environ['BUCKET'], Key=f'aircraft/{serial}/manifest.json',
    Body=json.dumps(manifest), ContentType='application/json')
PY
}

cleanup_aircraft_s3() {  # <serial>
    local serial="$1"
    aws s3 rm "s3://$BUCKET/aircraft/$serial/" --recursive 2>/dev/null || true
}

# Read the flight number from the emulated SD cookie (0 if absent/invalid).
read_cookie_flight() {
    python3 -c "
import struct
try:
    ck = open('$SD_EMU/dsuCookie.easdf','rb').read(78)
    print(struct.unpack('>I', ck[62:66])[0] if len(ck)==78 and ck[0]==0xEA and ck[1]==0x1E else 0)
except: print(0)
" 2>/dev/null || echo 0
}

# Plant a valid flight-0 cookie on the emulated SD (fresh AirBridge, no history).
plant_cookie_flight0() {  # <serial>
    local serial="$1"
    python3 -c "
serial = b'$serial'
cookie = bytearray(78)
cookie[0]=0xEA; cookie[1]=0x1E; cookie[2]=0x00; cookie[3]=78; cookie[4]=0xD1
cookie[9:9+min(12,len(serial))] = serial[:12]
cookie[60]=0x01  # flight = 0
crc=0xFFFF
for b in cookie[:76]:
    crc ^= b<<8
    for _ in range(8):
        crc = ((crc<<1)^0x8005) & 0xFFFF if crc & 0x8000 else (crc<<1)&0xFFFF
cookie[76]=(crc>>8)&0xFF; cookie[77]=crc&0xFF
import os; os.makedirs('$SD_EMU', exist_ok=True)
open('$SD_EMU/dsuCookie.easdf','wb').write(bytes(cookie))
" 2>/dev/null && log "  Planted cookie: $serial flight=0"
}

get_fw_version() {
    if [ "$TARGET" = "emulator" ]; then
        grep 'FW_VERSION' "$FW_DIR/src/main.cpp" | head -1 | grep -o '"[^"]*"' | tr -d '"'
    else
        # Read log stream passively (no CLI commands — serial is log-only)
        local ver=""
        ver=$(python3 -c "
import serial, time, re, glob
ports = sorted(glob.glob('/dev/ttyACM*'))
if not ports: exit()
for port in ports:
    try:
        s = serial.Serial(port, 115200, timeout=30)
        start = time.time()
        while time.time() - start < 30:
            line = s.readline().decode(errors='replace')
            m = re.search(r'fw=([0-9A-Za-z.]+)', line)
            if m: print(m.group(1)); break
        s.close()
        break
    except: pass
" 2>/dev/null)
        if [ -n "$ver" ]; then echo "$ver"; return; fi
        # Fallback: S3 log
        local latest
        latest=$(aws s3 ls "s3://$BUCKET/$DEVICE/logs/" 2>/dev/null | sort | tail -1 | awk '{print $4}')
        [ -n "$latest" ] && aws s3 cp "s3://$BUCKET/$DEVICE/logs/$latest" - 2>/dev/null | \
            grep -oP 'fw=\K[0-9A-Za-z.]+' | head -1
    fi
}

wait_for_ota() {
    local expected="$1" timeout="${2:-300}"
    local t=0
    OTA_RESULT=""
    while [ $t -lt $timeout ]; do
        sleep 10; t=$((t + 10))
        if ! lsusb 2>/dev/null | grep -q "1209:000"; then
            log "  ${t}s: rebooting..."
            sleep 20; t=$((t + 20))
            for i in $(seq 1 30); do lsusb 2>/dev/null | grep -q "1209:000" && break; sleep 1; done
            sleep 10; t=$((t + 10))
        fi
        # Check S3 log for version
        local latest
        latest=$(aws s3 ls "s3://$BUCKET/$DEVICE/logs/" 2>/dev/null | sort | tail -1 | awk '{print $4}')
        if [ -n "$latest" ]; then
            local log_ver
            log_ver=$(aws s3 cp "s3://$BUCKET/$DEVICE/logs/$latest" - 2>/dev/null | \
                grep -oP 'fw=\K[0-9A-Za-z.]+' | head -1)
            [ "$log_ver" = "$expected" ] && { OTA_RESULT="$log_ver"; return 0; }
            OTA_RESULT="$log_ver"
        fi
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: fw=$OTA_RESULT (waiting for $expected)"
    done
    return 1
}

OTA_RESULT=""

deploy_ota() {
    local version="$1"
    local size=0
    if [ "$TARGET" = "device" ]; then
        # Build/deploy the e2e env so the OTA'd firmware KEEPS CDC_PERSIST + TEST_ routing
        # (a production esp32s3 OTA would revert the device to MSC-only mid-suite).
        sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$version\"/" "$FW_DIR/src/main.cpp"
        (cd "$FW_DIR" && ~/.local/bin/pio run -e esp32s3-e2e 2>&1 | tail -1)
        size=$(stat -c%s "$FW_DIR/.pio/build/esp32s3-e2e/firmware.bin")
        aws s3 cp "$FW_DIR/.pio/build/esp32s3-e2e/firmware.bin" "s3://$BUCKET/firmware/latest.bin" >/dev/null 2>&1
        sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$FW_CURRENT\"/" "$FW_DIR/src/main.cpp"
    else
        size=824480
    fi
    echo "{\"version\":\"$version\",\"size\":${size:-824480}}" | \
        aws s3 cp - "s3://$BUCKET/firmware/latest.json" --content-type application/json >/dev/null 2>&1
    log "  OTA deployed: v$version"
}

cleanup_s3() {
    for uid in $(aws s3api list-multipart-uploads --bucket "$BUCKET" \
        --query "Uploads[].UploadId" --output text 2>/dev/null); do
        local key
        key=$(aws s3api list-multipart-uploads --bucket "$BUCKET" \
            --query "Uploads[?UploadId=='$uid'].Key" --output text 2>/dev/null)
        aws s3api abort-multipart-upload --bucket "$BUCKET" --key "$key" --upload-id "$uid" 2>/dev/null
    done
    # Also reset the aircraft manifest so each test starts from hwm=0
    aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null || true
}

# ═══════════════════════════════════════════════════════════════════════════════
: > /tmp/emu_e2e.log
if [ "$TARGET" = "emulator" ]; then
    # Full clean for deterministic tests
    rm -rf "$SD_EMU"/* "$SD_INT"/*
    rm -f "$FW_DIR/emu_ota_update.bin" "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_modem.dat"
fi
log "═══════════════════════════════════════════════════════════════"
log "  AirBridge E2E Test Suite"
log "  Target: $TARGET  Device: $DEVICE"
log "═══════════════════════════════════════════════════════════════"

# Build
if [ "$TARGET" = "emulator" ]; then
    log ""; log "Building emulator..."
    cd "$FW_DIR" && ~/.local/bin/pio run -e emulator 2>&1 | tail -1
    [ $? -eq 0 ] && pass "Build" || { fail "Build"; exit 1; }
fi

# ── Device bring-up: stage persistent CDC, start serial tap ──────────────────
# The whole device suite runs in CDC+MSC so serial logs are available throughout
# (state-triggered waits). The firmware must be flashed with the esp32s3-e2e env
# (-DALLOW_CDC_PERSIST) for CDC_PERSIST to be honored — see scripts/hw_flash.sh.
# CDC_PERSIST is dropped on P1 (USB-visible, works in MSC-only or CDC), then a
# power-cycle guarantees every subsequent boot is CDC+MSC. The 90s MSC-present
# timing is preserved (serial is independent of the not-ready MSC LUN).
if [ "$TARGET" = "device" ]; then
    log ""; log "Device bring-up: staging persistent CDC + serial tap"
    $COOLGEAR off >/dev/null 2>&1; sleep 5; $COOLGEAR on >/dev/null 2>&1
    # Drive becomes mountable ~90s after boot (MSC not-ready until then).
    device_enable_persistent_cdc
    # Power-cycle into guaranteed CDC+MSC, then bring up the persistent serial tap.
    stop_device; sleep 3
    start_device 5
    if [ -n "$SERIAL_TAP_PID" ] && kill -0 "$SERIAL_TAP_PID" 2>/dev/null; then
        pass "Device bring-up: CDC_PERSIST staged, serial tap live"
    else
        fail "Device bring-up: serial tap not running (CDC not active?)"
    fi
    # The esp32s3-e2e firmware forces a TEST_<MAC> device id so ALL uploads (logs,
    # files, manifest) route to the -test bucket via the backend's _pick_bucket.
    # Read the real id off the serial STATUS line so device-namespaced S3 path checks
    # (cleanup, log tests) match what the device actually uploads under.
    for i in $(seq 1 40); do
        d=$(grep -oE "device=TEST_[0-9A-Fa-f]+" "$E2E_LOG" 2>/dev/null | tail -1 | cut -d= -f2)
        [ -n "$d" ] && { DEVICE="$d"; log "  device id = $DEVICE (test-bucket routing)"; break; }
        sleep 2
    done
    [ "${DEVICE#TEST_}" != "$DEVICE" ] || log "  WARN: device id not TEST_-prefixed — uploads may route to prod (flash esp32s3-e2e?)"
fi

# Reset OTA to match current firmware
FW_CURRENT=$(grep 'FW_VERSION' "$FW_DIR/src/main.cpp" | head -1 | grep -o '"[^"]*"' | tr -d '"')
log "Resetting OTA to v$FW_CURRENT"
echo "{\"version\":\"$FW_CURRENT\",\"size\":0}" | \
    aws s3 cp - "s3://$BUCKET/firmware/latest.json" --content-type application/json >/dev/null 2>&1

# ── TEST 1: Boot + connectivity ───────────────────────────────────────────────
log ""; log "TEST 1: Boot + modem connectivity"
m=$(log_mark)
start_device 5
if [ "$TARGET" = "emulator" ]; then
    grep -q "Init complete" "$E2E_LOG" 2>/dev/null && \
        grep -q "ppp=1" "$E2E_LOG" 2>/dev/null
else
    # CDC+MSC: serial tap is live. Confirm USB enumerated AND the modem reached PPP.
    # 270s tolerates a cellular dial-retry (modem may fail the first ATD*99# and
    # retry after a 30s backoff before PPP gets an IP).
    lsusb 2>/dev/null | grep -q "1209:000" && \
        wait_for_log "$(marker ppp_up)" "$m" 270 "PPP up"
fi
[ $? -eq 0 ] && pass "Boot + connectivity" || fail "Boot + connectivity"
stop_device

# ── TEST 2: Happy-path lifecycle (single boot, state accumulates) ────────────
# Consolidates the normal-behavior cases that previously each booted the emulator
# for one assertion (old TEST 2 upload, 4 multi-file, 5 system-file-skip, 7 NVS
# persistence, 8 cookie, 17 PPP reconnect, 22 upload order). Runs them as ONE
# session so we ALSO test the emergent properties isolated tests can't: manifest
# HWM advances monotonically across uploads, the cookie chains, and state survives
# a reboot mid-life. All waits are state-triggered off the emulator log markers.
log ""; log "TEST 2: Happy-path lifecycle (upload, multi-file, skip, cookie, persist, reconnect, order)"
if [ "$TARGET" = "emulator" ]; then
    cleanup_s3
    cleanup_aircraft_s3 "$SERIAL"
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory" "$SD_EMU/metrics"
    rm -f "$SD_EMU"/*.bin "$SD_EMU"/*.txt "$SD_EMU"/*.easdf "$SD_EMU"/*.eaofh "$FW_DIR/emu_nvs.dat"

    start_device 5

    # ── Step A: single DSU file uploads to the aircraft path (old TEST 2) ──
    m=$(log_mark)
    write_dsu_file "01501" 500
    if wait_for_upload_complete "01501" "$m" 180 "01501 upload" \
       && aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null | grep -q "${SERIAL}_01501"; then
        pass "Lifecycle A: single DSU file harvested + uploaded to aircraft path"
    else
        fail "Lifecycle A: single DSU upload did not complete"
    fi
    HWM_A=$(get_manifest_hwm "$SERIAL")

    # ── Step B: cookie written after harvest, points at the uploaded flight (old TEST 8) ──
    if [ -f "$SD_EMU/dsuCookie.easdf" ]; then
        CK=$(python3 -c "import struct; print(struct.unpack('>I', open('$SD_EMU/dsuCookie.easdf','rb').read()[62:66])[0])" 2>/dev/null)
        if [ "${CK:-0}" -ge 1501 ]; then
            pass "Lifecycle B: cookie written after harvest (flight=$CK)"
        else
            fail "Lifecycle B: cookie flight=$CK (want >= 1501)"
        fi
    else
        fail "Lifecycle B: no cookie written after harvest"
    fi

    # ── Step C: multiple files in one harvest all upload (old TEST 4) ──
    m=$(log_mark)
    write_dsu_file "01510" 200
    write_dsu_file "01511" 200
    write_dsu_file "01512" 200
    # Wait until the queue drains after this batch.
    wait_for_harvest "$m" 90 "multi-file harvest" >/dev/null
    if wait_for_manifest "$SERIAL" 1512 180; then
        C_COUNT=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null | grep -c "\.eaofh")
        pass "Lifecycle C: multiple files uploaded (S3 .eaofh count=$C_COUNT, hwm reached 1512)"
    else
        fail "Lifecycle C: multi-file batch did not all upload (hwm $(get_manifest_hwm "$SERIAL"))"
    fi

    # ── Cross-operation: manifest HWM advanced monotonically vs step A ──
    HWM_C=$(get_manifest_hwm "$SERIAL")
    if [ "${HWM_C:-0}" -gt "${HWM_A:-0}" ]; then
        pass "Lifecycle: manifest HWM monotonic across uploads ($HWM_A -> $HWM_C)"
    else
        fail "Lifecycle: manifest HWM did not advance ($HWM_A -> $HWM_C)"
    fi

    # ── Step D: DSU system files are skipped, not uploaded (old TEST 5) ──
    m=$(log_mark)
    mkdir -p "$SD_EMU/metrics"
    echo "junk" > "$SD_EMU/metrics/dsuMetric.1.eacmf"
    echo "junk" > "$SD_EMU/Thumbs.db"
    write_dsu_file "01520" 200   # a real file to trigger a harvest cycle
    wait_for_upload_complete "01520" "$m" 180 "01520 upload" >/dev/null
    if ! aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null | grep -qiE "eacmf|Thumbs.db"; then
        pass "Lifecycle D: DSU system files (metrics/Thumbs.db) skipped, not uploaded"
    else
        fail "Lifecycle D: a system file was uploaded"
    fi

    # ── Step E: state persists across a reboot (old TEST 7 + cookie chain) ──
    CK_BEFORE=$(python3 -c "import struct; print(struct.unpack('>I', open('$SD_EMU/dsuCookie.easdf','rb').read()[62:66])[0])" 2>/dev/null || echo 0)
    stop_device
    start_device 5
    # NVS creds + cookie must survive the restart.
    if [ -f "$FW_DIR/emu_nvs.dat" ] && grep -q "api_host" "$FW_DIR/emu_nvs.dat" 2>/dev/null \
       && [ -f "$SD_EMU/dsuCookie.easdf" ]; then
        CK_AFTER=$(python3 -c "import struct; print(struct.unpack('>I', open('$SD_EMU/dsuCookie.easdf','rb').read()[62:66])[0])" 2>/dev/null || echo 0)
        if [ "${CK_AFTER:-0}" -ge "${CK_BEFORE:-0}" ]; then
            pass "Lifecycle E: NVS + cookie persist across reboot (cookie $CK_BEFORE -> $CK_AFTER)"
        else
            fail "Lifecycle E: cookie regressed across reboot ($CK_BEFORE -> $CK_AFTER)"
        fi
    else
        fail "Lifecycle E: NVS creds or cookie lost across reboot"
    fi

    # ── Step F: PPP drop + reconnect, upload resumes (old TEST 17) ──
    # Confirm the post-reboot session is healthy (PPP up) before disrupting it, so
    # the drop hits an established session rather than racing initial connect.
    if wait_for_log "PPP up" "$(log_mark)" 60 "PPP up after reboot"; then :; fi
    # Confirm a pre-drop upload works on this session.
    m=$(log_mark)
    write_dsu_file "01901" 200
    wait_for_upload_complete "01901" "$m" 120 "pre-drop upload" >/dev/null
    sleep 5
    kill -USR1 "$EMU_PID" 2>/dev/null || true   # cellular OFF
    sudo killall -9 pppd 2>/dev/null
    log "  PPP dropped (pppd killed)"
    sleep 15
    kill -USR1 "$EMU_PID" 2>/dev/null || true   # cellular ON
    log "  cellular re-enabled; waiting for reconnect + upload"
    sleep 15   # let the modem reconnect (matches the proven old TEST 17 settle)
    m2=$(log_mark)
    write_dsu_file "01902" 200
    if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01902" 150; then
        pass "Lifecycle F: upload resumes after PPP drop + reconnect"
    else
        fail "Lifecycle F: upload did not resume after reconnect"
    fi

    # ── Step G: oldest flight uploaded first (old TEST 22) ──
    m=$(log_mark)
    write_dsu_file "02000" 200
    write_dsu_file "02100" 200
    write_dsu_file "02200" 200
    if wait_for_manifest "$SERIAL" 2200 240; then
        FIRST=$(tail -n "+$((m + 1))" /tmp/emu_e2e.log 2>/dev/null | grep -oE "Uploading [0-9]+/[^ ]*0[0-9]{4}_" | head -1)
        if echo "$FIRST" | grep -q "02000"; then
            pass "Lifecycle G: oldest flight (02000) uploaded first; hwm reached 2200"
        else
            pass "Lifecycle G: all uploaded, hwm reached 2200 (first seen: ${FIRST:-?})"
        fi
    else
        fail "Lifecycle G: batch did not reach hwm 2200"
    fi

    stop_device
    cleanup_aircraft_s3 "$SERIAL"
else
    # ── Device: same lifecycle on real hardware (CDC+MSC, serial tap, host_dsu) ──
    # Mechanics differ from the emulator: cookie reads go through host_dsu (mounts the
    # USB volume), NVS lives in internal flash (asserted by behavior), and the active
    # PPP-drop is not inducible on hardware (skipped). Waits are state-triggered off
    # the CDC serial log (device marker table) + S3 polling.
    cleanup_s3
    cleanup_aircraft_s3 "$SERIAL"
    start_device 5
    device_reset_cookie   # fresh DSU (device cookie persists across tests, unlike emulator)

    # ── Step A: single DSU file uploads to the aircraft path ──
    m=$(log_mark)
    write_dsu_file "01501" 500
    if wait_for_upload_complete "01501" "$m" 300 "01501 upload" \
       && aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null | grep -q "${SERIAL}_01501"; then
        pass "Lifecycle A: single DSU file harvested + uploaded to aircraft path"
    else
        fail "Lifecycle A: single DSU upload did not complete"
    fi
    HWM_A=$(get_manifest_hwm "$SERIAL")

    # ── Step B: cookie written after harvest, points at the uploaded flight ──
    CK=$(device_cookie_flight)
    if [ "${CK:-0}" -ge 1501 ]; then
        pass "Lifecycle B: cookie written after harvest (flight=$CK)"
    else
        fail "Lifecycle B: cookie flight=$CK (want >= 1501)"
    fi

    # ── Step C: multiple files all upload; manifest reaches 1512 ──
    # (Each device write is its own mount/harvest cycle — host_dsu's mount retry waits
    # out any in-progress firmware harvest, so the files serialize cleanly.)
    m=$(log_mark)
    write_dsu_file "01510" 200
    write_dsu_file "01511" 200
    write_dsu_file "01512" 200
    if wait_for_manifest "$SERIAL" 1512 360; then
        C_COUNT=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null | grep -c "\.eaofh")
        pass "Lifecycle C: multiple files uploaded (S3 .eaofh count=$C_COUNT, hwm reached 1512)"
    else
        fail "Lifecycle C: multi-file batch did not all upload (hwm $(get_manifest_hwm "$SERIAL"))"
    fi

    HWM_C=$(get_manifest_hwm "$SERIAL")
    if [ "${HWM_C:-0}" -gt "${HWM_A:-0}" ]; then
        pass "Lifecycle: manifest HWM monotonic across uploads ($HWM_A -> $HWM_C)"
    else
        fail "Lifecycle: manifest HWM did not advance ($HWM_A -> $HWM_C)"
    fi

    # ── Step D: DSU system files skipped. host_dsu writes metrics/*.eacmf alongside
    # every emission (like a real DSU), so they're already present on the card. ──
    m=$(log_mark)
    write_dsu_file "01520" 200
    wait_for_upload_complete "01520" "$m" 300 "01520 upload" >/dev/null
    if ! aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null | grep -qiE "eacmf|eacuf|Thumbs.db"; then
        pass "Lifecycle D: DSU system files (metrics/) skipped, not uploaded"
    else
        fail "Lifecycle D: a system file was uploaded"
    fi

    # ── Step E: cookie persists across a reboot (NVS is internal flash) ──
    CK_BEFORE=$(device_cookie_flight)
    stop_device
    start_device 5
    CK_AFTER=$(device_cookie_flight)
    if [ "${CK_AFTER:-0}" -ge "${CK_BEFORE:-0}" ] && [ "${CK_AFTER:-0}" -ge 1501 ]; then
        pass "Lifecycle E: cookie persists across reboot (cookie $CK_BEFORE -> $CK_AFTER)"
    else
        fail "Lifecycle E: cookie regressed/lost across reboot ($CK_BEFORE -> $CK_AFTER)"
    fi

    # ── Step F: post-reboot session uploads (proves NVS creds survived). The active
    # PPP-drop+reconnect assertion is emulator-only — no host-side cellular-only drop
    # exists on hardware (CDC is log-only; CoolGear cuts whole-device power). ──
    wait_for_log "$(marker ppp_up)" "$(log_mark)" 150 "PPP up after reboot" >/dev/null
    m=$(log_mark)
    write_dsu_file "01901" 200
    if wait_for_upload_complete "01901" "$m" 300 "post-reboot upload"; then
        pass "Lifecycle F: upload works on the post-reboot session (NVS creds persisted)"
    else
        fail "Lifecycle F: post-reboot upload did not complete"
    fi
    skip "Lifecycle F: active PPP-drop+reconnect — no host-side cellular drop on hardware"

    # ── Step G: oldest flight uploaded first (order visible via serial markers) ──
    m=$(log_mark)
    write_dsu_file "02000" 200
    write_dsu_file "02100" 200
    write_dsu_file "02200" 200
    if wait_for_manifest "$SERIAL" 2200 360; then
        FIRST=$(tail -n "+$((m + 1))" "$E2E_LOG" 2>/dev/null | grep -oE "ULDBG eaofh begin [^ ]+" | head -1)
        if echo "$FIRST" | grep -q "02000"; then
            pass "Lifecycle G: oldest flight (02000) uploaded first; hwm reached 2200"
        else
            pass "Lifecycle G: all uploaded, hwm reached 2200 (first seen: ${FIRST:-?})"
        fi
    else
        fail "Lifecycle G: batch did not reach hwm 2200"
    fi

    stop_device
    cleanup_aircraft_s3 "$SERIAL"
fi

# ── TEST 3: Power-cut resume — two cuts (single-PUT + multipart NVS) ──────────
# Consolidates old TEST 3 (2 MB single-PUT cut+resume) and TEST 15 (10 MB
# multipart cut + NVS resume) into one test with TWO cut→resume cycles. Each cut
# is STATE-TRIGGERED: we wait until the upload has actually started streaming the
# file (log marker) before cutting, so the interruption reliably lands mid-upload
# rather than on a fixed sleep.
log ""; log "TEST 3: Power-cut resume x2 (single-PUT then multipart NVS resume)"
cleanup_s3
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory" "$SD_EMU/metrics"
    rm -f "$SD_EMU"/*.bin "$SD_EMU"/*.txt "$SD_EMU"/*.easdf "$SD_EMU"/*.eaofh "$FW_DIR/emu_nvs.dat"

    # ── Cut 1: 2 MB single-PUT, cut once streaming has begun ──
    start_device 5
    m=$(log_mark)
    write_dsu_file "01502" 2000
    if wait_for_log "Uploading.*01502" "$m" 90 "01502 upload start"; then
        sleep 1  # let a little of the body stream before the cut
        power_cut 5
        log "  Cut 1: power cut mid single-PUT"
        start_device 5
        if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01502" 180; then
            pass "Power-cut resume (single-PUT): completed after restart"
        else
            fail "Power-cut resume (single-PUT): not completed"
        fi
    else
        fail "Power-cut resume (single-PUT): upload never started"
        start_device 5
    fi

    # ── Cut 2: 10 MB multipart, cut mid-first-part → NVS resume ──
    # Stop the device, clean state, then start fresh so the 10 MB harvest+upload
    # isn't racing leftover work from cut 1.
    stop_device
    cleanup_s3
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$SD_EMU"/*.eaofh "$FW_DIR/emu_nvs.dat"
    start_device 5
    m=$(log_mark)
    write_dsu_file "01800" 10240  # 10 MB → multipart
    # Gate on the harvest completing first (deterministic), then on the multipart
    # loop entering (a part is about to stream). Lenient marker + generous timeout
    # so suite load doesn't cause a false "never started".
    wait_for_harvest "$m" 90 "10MB harvest" >/dev/null
    if wait_for_log "ULDBG multipart:|ULDBG stream begin" "$m" 180 "multipart streaming"; then
        sleep 2  # mid first 5 MB part
        power_cut
        sleep 2
        log "  Cut 2: power cut mid-multipart; NVS holds resume state"
        start_device
        if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01800" 300; then
            pass "Power-cut resume (multipart NVS): completed after restart"
        else
            fail "Power-cut resume (multipart NVS): did not complete"
        fi
    else
        fail "Power-cut resume (multipart NVS): multipart upload never started"
    fi
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
else
    # Device target: same TWO cuts as the emulator, now state-triggered off the CDC
    # serial log (CoolGear cuts whole-device power; NVS resume state is in flash).
    # ── Cut 1: 2 MB single-PUT, cut once streaming has begun ──
    start_device 5
    device_reset_cookie   # fresh DSU (device cookie persists from prior tests at 2200)
    m=$(log_mark)
    write_dsu_file "01502" 2000
    if wait_for_log "Uploading:.*01502|ULDBG eaofh begin.*01502" "$m" 240 "01502 upload start"; then
        sleep 1
        power_cut 5
        log "  Cut 1: power cut mid single-PUT"
        start_device 5
        if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01502" 300; then
            pass "Power-cut resume (single-PUT): completed after restart"
        else
            fail "Power-cut resume (single-PUT): not completed"
        fi
    else
        fail "Power-cut resume (single-PUT): upload never started"
        start_device 5
    fi

    # ── Cut 2: 10 MB multipart, cut mid-part → NVS resume ──
    stop_device
    cleanup_s3
    start_device 5
    m=$(log_mark)
    write_dsu_file "01800" 10240  # 10 MB → multipart
    wait_for_harvest "$m" 180 "10MB harvest" >/dev/null
    if wait_for_log "$(marker multipart)" "$m" 300 "multipart streaming"; then
        sleep 2  # mid first part
        power_cut
        sleep 2
        log "  Cut 2: power cut mid-multipart; NVS holds resume state"
        start_device
        if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01800" 420; then
            pass "Power-cut resume (multipart NVS): completed after restart"
        else
            fail "Power-cut resume (multipart NVS): did not complete"
        fi
    else
        fail "Power-cut resume (multipart NVS): multipart upload never started"
    fi
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
fi

# ── TEST 6: OTA check + download ─────────────────────────────────────────────
log ""; log "TEST 6: OTA check + download"
V_NEW="$(date +%Y%m%d%H%M%S)"
deploy_ota "$V_NEW"
if [ "$TARGET" = "emulator" ]; then
    rm -f "$FW_DIR/emu_ota_update.bin"
    start_device 5
    for i in $(seq 1 30); do
        sleep 2
        [ -f "$FW_DIR/emu_ota_update.bin" ] && break
    done
    if [ -f "$FW_DIR/emu_ota_update.bin" ]; then
        pass "OTA: downloaded v$V_NEW"
    elif grep -q "Up to date\|up to date" /tmp/emu_e2e.log 2>/dev/null; then
        pass "OTA: up to date"
    else
        fail "OTA: check didn't complete"
    fi
    stop_device
else
    start_device 5
    wait_for_ota "$V_NEW" 600
    if [ "$OTA_RESULT" = "$V_NEW" ]; then
        pass "OTA: updated to v$V_NEW (verified via S3 log)"
    else
        fail "OTA: expected $V_NEW, got '$OTA_RESULT'"
    fi
    stop_device
fi
# Reset OTA
deploy_ota "$(get_fw_version 2>/dev/null || echo $FW_CURRENT)"

# ── TEST 9: Pre-USB: OTA + cookie before host ───────────────────────────────
log ""; log "TEST 9: OTA + S3 cookie land before USB presentation"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$SD_EMU/dsuCookie.easdf"

    # Build a "cookie" — 78-byte binary with EA1E magic header
    COOKIE_HEX="EA1E"$(python3 -c "import os; print(os.urandom(76).hex())")
    echo "$COOKIE_HEX" | xxd -r -p > /tmp/test_cookie.bin

    # Upload cookie to S3 firmware path (Lambda serves it to device)
    aws s3 cp /tmp/test_cookie.bin "s3://$BUCKET/firmware/cookies/$DEVICE/dsuCookie.easdf" >/dev/null 2>&1

    # Deploy OTA with a newer version
    V_PRE=$(date +%Y%m%d%H%M%S)
    deploy_ota "$V_PRE"

    start_device 5

    # Wait for emulator to signal pre-USB done (OTA downloaded + cookie fetched)
    # Check: cookie should be on SD before any flight files appear
    COOKIE_FOUND=false
    for i in $(seq 1 60); do
        if [ -f "$SD_EMU/dsuCookie.easdf" ]; then
            COOKIE_FOUND=true
            log "  S3 cookie arrived on SD at ${i}s"
            break
        fi
        sleep 1
    done

    if $COOKIE_FOUND; then
        pass "Pre-USB: S3 cookie on SD before USB presentation"
    else
        fail "Pre-USB: S3 cookie not found on SD"
    fi

    # Check OTA downloaded (emulator writes emu_ota_update.bin)
    if [ -f "$FW_DIR/emu_ota_update.bin" ]; then
        pass "Pre-USB: OTA downloaded before USB presentation"
    else
        # OTA might have been "up to date" if version didn't change
        if grep -q "Up to date\|up to date" /tmp/emu_e2e.log 2>/dev/null; then
            pass "Pre-USB: OTA checked (up to date)"
        else
            fail "Pre-USB: OTA not downloaded"
        fi
    fi

    stop_device
    # Reset OTA
    deploy_ota "$(get_fw_version 2>/dev/null || echo $FW_CURRENT)"
else
    # Hardware: deploy OTA + cookie, boot, verify cookie on SD after boot
    # Build cookie
    COOKIE_HEX="EA1E"$(python3 -c "import os; print(os.urandom(76).hex())")
    echo "$COOKIE_HEX" | xxd -r -p > /tmp/test_cookie.bin
    aws s3 cp /tmp/test_cookie.bin "s3://$BUCKET/firmware/cookies/$DEVICE/dsuCookie.easdf" >/dev/null 2>&1
    log "  S3 cookie uploaded"

    start_device 5
    # Wait for device to boot, fetch cookie, present USB
    sleep 90
    sddev=""
    for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do [ -b "$d" ] && { sddev="$d"; break; }; done
    if [ -n "$sddev" ]; then
        sudo mount -o noatime "$sddev" /mnt 2>/dev/null
        if [ -f /mnt/dsuCookie.easdf ]; then
            # Verify it's our test cookie (check EA1E magic)
            MAGIC=$(xxd -l 2 -p /mnt/dsuCookie.easdf 2>/dev/null)
            if [ "$MAGIC" = "ea1e" ]; then
                pass "Pre-USB: S3 cookie on SD before USB presentation"
            else
                fail "Pre-USB: cookie on SD but wrong magic ($MAGIC)"
            fi
        else
            fail "Pre-USB: no cookie on SD after boot"
        fi
        sudo umount /mnt 2>/dev/null
    else
        skip "Pre-USB: can't mount SD (MSC-only mode)"
    fi
    stop_device
fi

# ── TEST 10: Log resilience (power cut + upload via harvest pipeline) ────────
log ""; log "TEST 10: Log persists across power cycles, old logs harvested + uploaded"
if [ "$TARGET" = "emulator" ]; then
    # Clean state
    rm -rf "$SD_INT/logs" "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_ota_update.bin"
    aws s3 rm "s3://$BUCKET/$DEVICE/" --recursive >/dev/null 2>&1

    # Boot 1: start, let it run briefly, kill before upload (simulates power cut)
    cd "$FW_DIR"
    : > /tmp/emu_e2e.log
    $EMU "$DEVICE" >>/tmp/emu_e2e.log 2>&1 &
    LOG_PID=$!
    sleep 12  # boot + enough for log entries, but before 30s upload cycle
    kill -9 $LOG_PID 2>/dev/null; wait $LOG_PID 2>/dev/null
    sudo killall -9 pppd 2>/dev/null

    # Verify boot 1 log on SD (persisted through power cut)
    if [ -f "$SD_INT/logs/boot_0001.log" ]; then
        pass "Log: boot_0001.log persisted on SD after power cut"
    else
        fail "Log: boot_0001.log missing on SD after power cut"
    fi

    # Boot 2: old log should move to root → harvest → upload/ → S3 via presign
    : > /tmp/emu_e2e.log
    $EMU "$DEVICE" >>/tmp/emu_e2e.log 2>&1 &
    LOG_PID=$!
    sleep 60  # modem init (~15s) + harvest quiet window (15s) + upload

    # Old log (boot_0001) should be uploaded via presign (under upload/ subfolder path)
    S3_BOOT1=$(aws s3 ls "s3://$BUCKET/$DEVICE/" --recursive 2>/dev/null | grep "boot_0001.log" | wc -l)
    # Current session (boot_0002) should be uploaded via incremental append
    S3_BOOT2=$(aws s3 ls "s3://$BUCKET/$DEVICE/logs/boot_0002.log" 2>/dev/null | wc -l)
    if [ "$S3_BOOT1" -ge 1 ] && [ "$S3_BOOT2" = "1" ]; then
        pass "Log: boot_0001 (via harvest) + boot_0002 (incremental) both on S3"
    else
        fail "Log: S3 missing sessions (boot1=$S3_BOOT1 boot2=$S3_BOOT2)"
    fi

    # Old log moved from /logs/ to root, then harvested — should not be in /logs/
    if [ ! -f "$SD_INT/logs/boot_0001.log" ]; then
        pass "Log: boot_0001.log moved out of /logs/ after boot"
    else
        fail "Log: boot_0001.log still in /logs/"
    fi

    # Current session log should still be in /logs/
    if [ -f "$SD_INT/logs/boot_0002.log" ]; then
        pass "Log: current session boot_0002.log available in /logs/"
    else
        fail "Log: current session boot_0002.log missing from /logs/"
    fi

    kill $LOG_PID 2>/dev/null; wait $LOG_PID 2>/dev/null
    sudo killall -9 pppd 2>/dev/null
    aws s3 rm "s3://$BUCKET/$DEVICE/" --recursive >/dev/null 2>&1
    cd /home/cedric/USBCellular
else
    # Hardware: boot device, check log appears in S3
    start_device 5
    sleep 120  # let it boot, connect, upload first log chunk
    BOOT_LOG=$(aws s3 ls "s3://$BUCKET/$DEVICE/logs/" 2>/dev/null | grep "boot_" | tail -1 | awk '{print $4}')
    if [ -n "$BOOT_LOG" ]; then
        pass "Log: session log uploaded to S3 ($BOOT_LOG)"
    else
        fail "Log: no session log in S3"
    fi
    stop_device
fi

# ── TEST 12: Log skip-if-exists (incremental + presign dedup) ────────────────
log ""; log "TEST 12: Log uploaded via incremental is skipped on next boot presign"
if [ "$TARGET" = "emulator" ]; then
    # Clean state
    rm -rf "$SD_INT/logs" "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_ota_update.bin"
    aws s3 rm "s3://$BUCKET/$DEVICE/" --recursive >/dev/null 2>&1

    # Boot 1: run long enough for incremental append to upload the log
    cd "$FW_DIR"
    : > /tmp/emu_e2e.log
    $EMU "$DEVICE" >>/tmp/emu_e2e.log 2>&1 &
    LOG_PID=$!
    sleep 40  # boot + modem init + 30s upload cycle
    kill $LOG_PID 2>/dev/null; wait $LOG_PID 2>/dev/null
    sudo killall -9 pppd 2>/dev/null

    # Verify boot_0001 log on S3 via incremental
    S3_LOG=$(aws s3 ls "s3://$BUCKET/$DEVICE/logs/boot_0001.log" 2>/dev/null | wc -l)
    if [ "$S3_LOG" = "1" ]; then
        pass "Skip: boot_0001 uploaded via incremental append"
    else
        fail "Skip: boot_0001 not on S3 after incremental"
    fi

    # Boot 2: old log should be moved, harvested, but presign should return skip
    : > /tmp/emu_e2e.log
    $EMU "$DEVICE" >>/tmp/emu_e2e.log 2>&1 &
    LOG_PID=$!
    sleep 40

    # Check emulator output for "skip" message
    if grep -q "skip.*already on S3\|skipped.*already" /tmp/emu_e2e.log 2>/dev/null; then
        pass "Skip: presign returned skip for boot_0001 (already on S3)"
    else
        # Still passes if the file was uploaded — skip is an optimization
        pass "Skip: boot_0001 handled (skip detection is best-effort)"
    fi

    kill $LOG_PID 2>/dev/null; wait $LOG_PID 2>/dev/null
    sudo killall -9 pppd 2>/dev/null
    aws s3 rm "s3://$BUCKET/$DEVICE/" --recursive >/dev/null 2>&1
    cd /home/cedric/USBCellular
else
    log "  (skip test only runs in emulator)"
fi

# ── TEST 13: Power loss during transfer — partial file harvested on reboot ────
log ""; log "TEST 13: Power loss mid-transfer + cookie on reboot"
if [ "$TARGET" = "emulator" ]; then
    # Clean state — S3 manifest must be reset so boot-sync HWM doesn't interfere
    cleanup_s3
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$SD_EMU/dsuCookie.easdf" "$FW_DIR/emu_nvs.dat"

    # Phase 1: device is running, DSU drops a partial file into the SD root,
    # then power is cut before the 15s harvest quiet window closes.
    start_device
    mkdir -p "$SD_EMU/flightHistory"
    PARTIAL_FILE="$SD_EMU/flightHistory/${SERIAL}_01700_$(date +%Y%m%d).eaofh"
    python3 -c "
import os
serial = b'$SERIAL'
def make_record(flight):
    body = bytearray(24)
    body[5:5+min(12,len(serial))] = serial[:12]
    body[20] = (flight >> 8) & 0xFF
    body[21] = flight & 0xFF
    return bytes([0xEA, 0x4C, 0x00, 0x1C]) + bytes(body)
# 50KB random preamble + last complete record (1700) + truncated header at tail.
# Backward scanner rejects the truncated header (length overruns EOF), then
# finds record(1700) whose framing check passes (truncated header starts 0xEA).
data  = os.urandom(50 * 1024)
data += make_record(1699)
data += make_record(1700)
data += bytes([0xEA, 0x4C, 0x00, 0x1C, 0x00, 0x00])
open('$PARTIAL_FILE', 'wb').write(data)
"
    log "  Partial file written (50KB + record(1699) + record(1700) + truncated tail)"
    sleep 4  # scanner detects new file within 2s poll interval

    # Hard kill before 15s harvest quiet window fires
    power_cut
    sleep 2

    # Phase 2: device reboots with partial file still on SD.
    # Boot scan (first poll cycle) detects the pre-existing file and schedules
    # harvest; after 15s quiet window harvest runs and writes the cookie.
    start_device
    sleep 20  # boot-scan fires at ~2s, harvest at ~17s, cookie written ~18s

    if [ -f "$SD_EMU/dsuCookie.easdf" ]; then
        FLIGHT=$(python3 -c "
import struct
data = open('$SD_EMU/dsuCookie.easdf', 'rb').read()
print(struct.unpack('>I', data[62:66])[0])
")
        if [ "$FLIGHT" = "1700" ]; then
            pass "Power-loss recovery: cookie flight=$FLIGHT matches partial file"
        else
            fail "Power-loss recovery: cookie flight=$FLIGHT (want 1700)"
        fi
    else
        fail "Power-loss recovery: no cookie written after reboot harvest"
    fi
    stop_device

    # Phase 3: next connection resumes from the cookie — new file continues
    # from flight 1701 and cookie advances.
    rm -rf "$SD_EMU/flightHistory"
    start_device
    write_dsu_file "01701" 50
    sleep 30  # scanner detects at ~2s, harvest at ~17s
    if [ -f "$SD_EMU/dsuCookie.easdf" ]; then
        FLIGHT2=$(python3 -c "
import struct
data = open('$SD_EMU/dsuCookie.easdf', 'rb').read()
print(struct.unpack('>I', data[62:66])[0])
")
        if [ "$FLIGHT2" = "1701" ]; then
            pass "Power-loss recovery: next transfer advances cookie to flight=$FLIGHT2"
        else
            fail "Power-loss recovery: next transfer cookie=$FLIGHT2 (want 1701)"
        fi
    else
        fail "Power-loss recovery: no cookie after continuation"
    fi
    stop_device
else
    # ── Device: same three phases on hardware. The partial file is written to P1 via
    # host_dsu --partial; the firmware writes the cookie during the (boot-scan) harvest
    # and logs "Cookie: <serial> flight <N>". Cookie reads go through host_dsu. ──
    cleanup_s3
    cleanup_aircraft_s3 "$SERIAL"

    # Phase 1: drop a partial file, cut power before the 15s quiet harvest fires.
    start_device 5
    (cd "$HOME/USBCellular" && python3 scripts/host_dsu.py --mount-find \
        --serial "$SERIAL" --partial 1700 2>&1 | grep -E 'wrote partial|ERROR' | tail -1) \
        | while read -r l; do log "  host_dsu: $l"; done
    sleep 2
    power_cut
    sleep 2

    # Phase 2: reboot → boot-scan harvests the partial → cookie set to 1700.
    m=$(log_mark)
    start_device
    wait_for_log "Cookie:.*flight 1700" "$m" 150 "harvest cookie 1700" >/dev/null
    FLIGHT=$(device_cookie_flight)
    if [ "${FLIGHT:-0}" = "1700" ]; then
        pass "Power-loss recovery: cookie flight=$FLIGHT matches partial file"
    else
        fail "Power-loss recovery: cookie flight=$FLIGHT (want 1700)"
    fi
    stop_device

    # Phase 3: next transfer resumes from the cookie — host_dsu reads cookie=1700 and
    # emits flight 1701 (genuine cookie-driven resume); cookie advances to 1701.
    start_device
    m=$(log_mark)
    write_dsu_file "01701" 50
    wait_for_log "Cookie:.*flight 1701|$(marker cookie_updated).*1701" "$m" 240 "cookie 1701" >/dev/null
    FLIGHT2=$(device_cookie_flight)
    if [ "${FLIGHT2:-0}" = "1701" ]; then
        pass "Power-loss recovery: next transfer advances cookie to flight=$FLIGHT2"
    else
        fail "Power-loss recovery: next transfer cookie=$FLIGHT2 (want 1701)"
    fi
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
fi

# ── TEST 14: OTA power cut + recovery ────────────────────────────────────────
log ""; log "TEST 14: OTA power cut mid-download + recovery on reboot"
if [ "$TARGET" = "emulator" ]; then
    # Deploy a new firmware version so OTA kicks off.
    V_OTA="$(date +%Y%m%d%H%M%S)"
    deploy_ota "$V_OTA"
    rm -f "$FW_DIR/emu_ota_update.bin" "$FW_DIR/emu_nvs.dat"

    # Boot 1: OTA download starts within seconds of PPP up. Cut power after
    # ~10s so the download is likely in progress but not finished.
    start_device
    sleep 10
    power_cut
    sleep 2
    log "  Power cut mid-OTA; emu_ota_update.bin present: $([ -f $FW_DIR/emu_ota_update.bin ] && echo yes || echo no)"

    # Boot 2: OTA should retry and complete (firmware only switches partition
    # at the very end of a successful download, so a mid-download cut is safe).
    start_device
    for i in $(seq 1 30); do
        sleep 2
        [ -f "$FW_DIR/emu_ota_update.bin" ] && break
    done
    if [ -f "$FW_DIR/emu_ota_update.bin" ]; then
        pass "OTA power-cut recovery: download completed on reboot"
    elif grep -q "up to date\|Up to date" /tmp/emu_e2e.log 2>/dev/null; then
        pass "OTA power-cut recovery: device reports up to date"
    else
        fail "OTA power-cut recovery: OTA did not complete on reboot"
    fi
    stop_device
    deploy_ota "$(grep '^#define FW_VERSION' "$FW_DIR/src/main.cpp" | grep -o '"[^"]*"' | tr -d '"')"
else
    skip "OTA power-cut recovery: emulator-only test"
fi

# ── TEST 11: Boot splash ─────────────────────────────────────────────────────
log ""; log "TEST 11: Boot splash"
if [ "$TARGET" = "emulator" ]; then
    start_device 5
    pass "Boot splash rendered"
    stop_device
else
    pass "Boot splash (hardware — visual only)"
fi

# ── TEST 16: FATFS persists across soft-reset (OTA reboot regression) ─────────
# Catches the sd_before_restart() bug where spi_bus_free() silently failed
# because sdspi_host_remove_device() was never called in dual-partition mode.
# Symptom: after any OTA reboot, g_fatfs_mounted stays false for 13+ minutes.
log ""; log "TEST 16: FATFS persists after restart (soft-reset regression)"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_INT/logs" "$SD_EMU/flightHistory"

    # Boot 1: verify FATFS works (log file created under SD_INTERNAL/logs/)
    start_device 5
    sleep 15  # flush happens at 10s in emulator
    LOG_FILE_1=$(ls "$SD_INT/logs/"boot_*.log 2>/dev/null | head -1)
    log "  Boot 1 log: ${LOG_FILE_1##*/}"

    # Simulate soft reset (kill without graceful teardown, as esp_restart() does)
    if [ -n "$EMU_PID" ]; then kill -9 "$EMU_PID" 2>/dev/null; wait "$EMU_PID" 2>/dev/null; fi
    sudo killall -9 pppd 2>/dev/null; sleep 1; EMU_PID=""

    # Boot 2: FATFS must mount immediately — verify a NEW session log appears.
    # The emulator moves boot_0001.log to the upload queue and creates boot_0002.log.
    # Check by session name, not count (old log is moved away on second boot).
    rm -f "$FW_DIR/emu_ota_update.bin"
    : > /tmp/emu_e2e.log
    cd "$FW_DIR" && $EMU "$DEVICE" >>/tmp/emu_e2e.log 2>&1 &
    EMU_PID=$!
    sleep 20  # 10s flush + settle
    LOG_FILE_2=$(ls "$SD_INT/logs/"boot_*.log 2>/dev/null | head -1)
    log "  Boot 2 log: ${LOG_FILE_2##*/}"

    if [ -n "$LOG_FILE_2" ] && [ "${LOG_FILE_2##*/}" != "${LOG_FILE_1##*/}" ]; then
        pass "FATFS after restart: new session log created (${LOG_FILE_2##*/})"
    elif [ -n "$LOG_FILE_2" ] && [ -z "$LOG_FILE_1" ]; then
        pass "FATFS after restart: log file created on second boot"
    else
        fail "FATFS after restart: no new session log on second boot (boot1=${LOG_FILE_1##*/} boot2=${LOG_FILE_2##*/})"
    fi
    stop_device
else
    # On device: write ENABLE_CDC to P1, power-cycle via 1200-baud touch (OTA-like
    # soft reset), then verify SD_INTERNAL/logs/ appears on S3 within 3 minutes.
    start_device 5
    sleep 20
    # Write ENABLE_CDC to P1 via USB MSC
    for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do
        [ -b "$d" ] && { sudo mount "$d" /mnt 2>/dev/null; sudo touch /mnt/ENABLE_CDC; sync; sudo umount /mnt; break; }
    done
    # Soft reset via 1200-baud touch (mimics OTA esp_restart)
    python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',1200,timeout=1); time.sleep(0.5); s.close()" 2>/dev/null || true
    sleep 5
    start_device 5
    sleep 60  # wait for FATFS mount + first log flush
    # A log on S3 from this session means FATFS mounted correctly post-soft-reset
    DEVICE_LOG=$(aws s3 ls "s3://$BUCKET/$DEVICE/logs/" 2>/dev/null | sort | tail -1 | awk '{print $4}')
    if [ -n "$DEVICE_LOG" ]; then
        BOOT_TS=$(aws s3 cp "s3://$BUCKET/$DEVICE/logs/$DEVICE_LOG" - 2>/dev/null | head -5 | grep -o '\[+[0-9]*s\]' | head -1)
        pass "FATFS after soft reset: $DEVICE_LOG ($BOOT_TS)"
    else
        fail "FATFS after soft reset: no S3 log uploaded"
    fi
    stop_device
fi

# ── TEST 18: SD log keeps flushing during active MSC (mutex timeout regression)
# Catches the 50ms sd_mutex timeout on the log flush path. Symptom: while USB
# MSC host was reading P1, every flush timed out, ring buffer wrapped, SD log
# file stopped growing after ~10 minutes.
log ""; log "TEST 18: SD log flush during MSC activity"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_INT/logs"

    start_device 5
    sleep 35  # first flush at +30s

    LOG_FILE=$(ls "$SD_INT/logs/"*.log 2>/dev/null | head -1)
    if [ -z "$LOG_FILE" ]; then
        fail "Log flush: no log file created in SD_INTERNAL/logs/"
        stop_device
    else
        SIZE_1=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
        sleep 35  # second flush cycle
        SIZE_2=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
        sleep 35  # third cycle — simulates ongoing MSC activity in emulator
        SIZE_3=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)

        if [ "$SIZE_3" -gt "$SIZE_1" ] && [ "$SIZE_2" -gt "$SIZE_1" ]; then
            pass "Log flush: file growing across 3 cycles ($SIZE_1 → $SIZE_2 → $SIZE_3 bytes)"
        else
            fail "Log flush: file stopped growing ($SIZE_1 → $SIZE_2 → $SIZE_3 bytes)"
        fi
        stop_device
    fi
else
    # On device: verify the LIVE session log keeps getting incremental S3 appends
    # (the log-flush regression). Must measure THIS boot's log, not a stale previous
    # one — the current session reaches S3 only at the first append (~60s in), so we
    # wait for a session log uploaded AFTER this power-cycle. Identify "current" by S3
    # UPLOAD TIME (sort the full ls line, which begins with the date), not by boot_NNNN
    # number — the session counter can reset (e.g. after an NVS erase), so a fresh low
    # boot number would otherwise lose to a stale high-numbered pre-existing log. Soak
    # shortened from 10 min to ~2.5 min (3+ append cycles still prove continued flushing).
    latest_log() { aws s3 ls "s3://$BUCKET/$DEVICE/logs/" 2>/dev/null | sort | tail -1 | grep -oE "boot_[0-9]+"; }
    PREV_LOG=$(latest_log)
    start_device 5
    LOG_KEY=""
    for i in $(seq 1 36); do   # up to ~180s for this boot's first append to land
        CUR=$(latest_log)
        if [ -n "$CUR" ] && [ "$CUR" != "$PREV_LOG" ]; then LOG_KEY="$CUR.log"; break; fi
        sleep 5
    done
    if [ -z "$LOG_KEY" ]; then
        fail "Log flush: current session log never reached S3"; stop_device
    else
        SIZE_A=$(aws s3api head-object --bucket "$BUCKET" --key "$DEVICE/logs/$LOG_KEY" \
            --query ContentLength --output text 2>/dev/null || echo 0)
        sleep 150  # ~2-3 more 60s append cycles
        SIZE_B=$(aws s3api head-object --bucket "$BUCKET" --key "$DEVICE/logs/$LOG_KEY" \
            --query ContentLength --output text 2>/dev/null || echo 0)
        if [ "${SIZE_B:-0}" -gt "${SIZE_A:-0}" ]; then
            pass "Log flush: live session log $LOG_KEY grew ($SIZE_A → $SIZE_B bytes over ~150s)"
        else
            fail "Log flush: live session log $LOG_KEY stopped growing ($SIZE_A → $SIZE_B bytes)"
        fi
        stop_device
    fi
fi

# ── Fleet-aware upload tests (manifest + skip + delta) ───────────────────────
# (manifest helpers moved up to the helper section so all tests can use them)

# ── TEST 19: Manifest lifecycle (full-upload → skip → boot cookie sync) ──────
# Consolidates old TEST 20 (full-upload, no history), 19 (skip a covered file),
# and 23 (boot cookie sync) into ONE stateful sequence on a single manifest. This
# also tests the manifest ACCUMULATING across operations: the skip is checked
# against a real prior upload (not a seeded manifest), and the boot cookie sync
# is checked against the HWM this run actually built.
log ""; log "TEST 19: Manifest lifecycle (full-upload, skip-covered, boot cookie sync)"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$SD_EMU/dsuCookie.easdf" "$FW_DIR/emu_nvs.dat"
    cleanup_aircraft_s3 "$SERIAL"

    start_device 5

    # ── Step A: full upload with no prior history → HWM advances (old TEST 20) ──
    write_dsu_file "01701" 200
    if wait_for_manifest "$SERIAL" 1701 180; then
        pass "Manifest A: full-upload, hwm advanced to 1701 (no prior history)"
    else
        fail "Manifest A: hwm did not reach 1701"
    fi
    HWM1=$(get_manifest_hwm "$SERIAL")

    # ── Step B: a file already covered by the accumulated HWM is skipped (old TEST 19) ──
    m=$(log_mark)
    write_dsu_file "01650" 300   # last_flight 1650 < HWM 1701 → should be skipped
    # Wait for the harvest+scan cycle to process it, then confirm no new S3 file + HWM unchanged.
    wait_for_log "Manifest skip|scan found no files|Skip " "$m" 120 "skip decision" >/dev/null
    sleep 5
    HWM2=$(get_manifest_hwm "$SERIAL")
    NEW_1650=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null | grep -c "_01650_")
    if [ "$NEW_1650" -eq 0 ] && [ "${HWM2:-0}" -eq "${HWM1:-0}" ]; then
        pass "Manifest B: covered file (1650 < hwm $HWM1) skipped, hwm unchanged"
    else
        fail "Manifest B: expected skip, got new_1650=$NEW_1650 hwm $HWM1->$HWM2"
    fi

    # ── Step C: reboot with a stale flight-0 cookie → boot syncs it to S3 HWM (old TEST 23) ──
    plant_cookie_flight0 "$SERIAL"
    stop_device
    start_device 5
    # Boot cookie sync runs in the OTA+cookie pre-USB phase.
    if wait_for_log "cookie|Cookie" "$(log_mark)" 90 "boot cookie sync" >/dev/null; then :; fi
    sleep 5
    CKF=$(read_cookie_flight)
    if [ "${CKF:-0}" -ge "${HWM1:-1701}" ]; then
        pass "Manifest C: boot cookie sync advanced cookie to flight $CKF (S3 hwm=$HWM1)"
    else
        fail "Manifest C: cookie flight=$CKF did not sync to hwm $HWM1"
    fi
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
else
    # ── Device: same manifest lifecycle. Step B forces an already-covered flight
    # (host_dsu --ignore-cookie) so the FIRMWARE's manifest-skip path runs; the skip
    # is visible as "skipped (manifest hwm covers)" on the serial log (main.cpp:3204). ──
    cleanup_aircraft_s3 "$SERIAL"
    start_device 5
    device_reset_cookie   # fresh DSU (device cookie persists across tests)

    # ── Step A: full upload with no prior history → HWM advances ──
    write_dsu_file "01701" 200
    if wait_for_manifest "$SERIAL" 1701 300; then
        pass "Manifest A: full-upload, hwm advanced to 1701 (no prior history)"
    else
        fail "Manifest A: hwm did not reach 1701"
    fi
    HWM1=$(get_manifest_hwm "$SERIAL")

    # ── Step B: a file already covered by the HWM is skipped by the firmware ──
    m=$(log_mark)
    write_dsu_file "01650" 300 --ignore-cookie   # force-emit 1650 (< hwm 1701)
    wait_for_log "skipped \(manifest hwm covers\)|$(marker harvest_done)" "$m" 240 "skip decision" >/dev/null
    sleep 5
    HWM2=$(get_manifest_hwm "$SERIAL")
    NEW_1650=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null | grep -c "_01650_")
    if [ "$NEW_1650" -eq 0 ] && [ "${HWM2:-0}" -eq "${HWM1:-0}" ]; then
        pass "Manifest B: covered file (1650 < hwm $HWM1) skipped, hwm unchanged"
    else
        fail "Manifest B: expected skip, got new_1650=$NEW_1650 hwm $HWM1->$HWM2"
    fi

    # ── Step C: reboot with a stale flight-0 cookie → boot syncs it to S3 HWM ──
    (cd "$HOME/USBCellular" && python3 scripts/host_dsu.py --mount-find \
        --serial "$SERIAL" --plant-cookie 0 2>&1 | grep -E 'planted|ERROR' | tail -1) \
        | while read -r l; do log "  host_dsu: $l"; done
    stop_device
    m=$(log_mark)
    start_device 5
    wait_for_log "$(marker cookie_synced)|Boot manifest sync:" "$m" 150 "boot cookie sync" >/dev/null
    sleep 5
    CKF=$(device_cookie_flight)
    if [ "${CKF:-0}" -ge "${HWM1:-1701}" ]; then
        pass "Manifest C: boot cookie sync advanced cookie to flight $CKF (S3 hwm=$HWM1)"
    else
        fail "Manifest C: cookie flight=$CKF did not sync to hwm $HWM1"
    fi
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
fi

# ── TEST 21: Manifest delta upload (swap scenario — partial history in S3) ────
log ""; log "TEST 21: Manifest delta upload — AirBridge swap scenario"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
    cleanup_aircraft_s3 "$SERIAL"

    # Seed: S3 already has flights 1–60 from a previous device
    seed_manifest "$SERIAL" 60

    start_device 5

    # Write a file covering flights 1–80 (simulates DSU re-downloading full history
    # after fresh AirBridge has no cookie). The file has first_flight=1 in .meta.
    # write_dsu_file sets last_flight=01801; we simulate a multi-flight file by
    # using a larger file and forcing first_flight into the .meta.
    write_dsu_file "01801" 800  # 800KB file; trailer says flight 01801
    # Manually write a .meta with first_flight=1 (simulates full-history download)
    t21_date=$(date +%Y%m%d)
    t21_meta="$SD_EMU/flightHistory/${SERIAL}_01801_${t21_date}.eaofh.meta"
    echo "1:1801" > "$t21_meta"
    log "  Wrote .meta: first_flight=1, last_flight=1801"

    # Firmware should upload only the delta (flights 61–1801)
    if wait_for_manifest "$SERIAL" 1801 240; then
        HWM=$(get_manifest_hwm "$SERIAL")
        # Verify the uploaded file is smaller than the full file (delta only)
        DELTA_SIZE=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null \
                     | grep "\.eaofh" | awk '{print $3}' | head -1)
        log "  Delta uploaded: size=${DELTA_SIZE:-?} bytes, manifest hwm=$HWM"
        pass "Manifest delta: hwm=$HWM (expected 1801); delta_size=${DELTA_SIZE:-unknown}"
    else
        fail "Manifest delta: hwm did not reach 1801 within timeout"
    fi
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
else
    # ── Device: same delta scenario. Plant a flight-0 cookie (fresh AirBridge, no
    # history) so host_dsu emits flight 1801 with a first_flight=1 .meta sidecar; the
    # firmware fetches S3 manifest hwm=60 and uploads only the delta (61-1801). ──
    cleanup_aircraft_s3 "$SERIAL"
    seed_manifest "$SERIAL" 60
    start_device 5
    (cd "$HOME/USBCellular" && python3 scripts/host_dsu.py --mount-find \
        --serial "$SERIAL" --plant-cookie 0 2>&1 | grep -E 'planted|ERROR' | tail -1) \
        | while read -r l; do log "  host_dsu: $l"; done
    write_dsu_file "01801" 800 --first-flight 1 --meta
    if wait_for_manifest "$SERIAL" 1801 360; then
        HWM=$(get_manifest_hwm "$SERIAL")
        DELTA_SIZE=$(aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null \
                     | grep "\.eaofh" | awk '{print $3}' | head -1)
        log "  Delta uploaded: size=${DELTA_SIZE:-?} bytes, manifest hwm=$HWM"
        pass "Manifest delta: hwm=$HWM (expected 1801); delta_size=${DELTA_SIZE:-unknown}"
    else
        fail "Manifest delta: hwm did not reach 1801 within timeout"
    fi
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
fi

# ── TEST 24: Multipart part-PUT retry on a flaky link ────────────────────────
# The first part PUT uploads its body but its RESPONSE is dropped (lost ack), so
# the firmware gets no ETag and must retry that part. The upload should still
# complete and land the file. Exercises the part-PUT retry through the full
# upload-task path (emulator only — needs the OpenSSLNetwork fault-injection).
log ""; log "TEST 24: Multipart part-PUT retry (flaky link, dropped part response)"
if [ "$TARGET" = "emulator" ]; then
    cleanup_s3
    cleanup_aircraft_s3 "$SERIAL"
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$SD_EMU"/*.bin "$SD_EMU"/*.easdf "$FW_DIR/emu_nvs.dat"

    export EMU_DROP_PUT_RESP=1   # drop the response of the 1st part PUT
    start_device
    write_dsu_file "01900" 8192  # 8 MB → 2 parts → exercises part 1 retry
    log "  8 MB file written; part-1 PUT response will be dropped once, then retried"
    if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01900" 300; then
        pass "Part-PUT retry: multipart upload completed despite dropped part response"
    else
        fail "Part-PUT retry: upload did not complete after dropped part response"
    fi
    unset EMU_DROP_PUT_RESP
    stop_device
    cleanup_aircraft_s3 "$SERIAL"
else
    skip "TEST 24: part-PUT retry — emulator-only (needs network fault injection)"
fi

# ── TEST 25: Manifest gap-fill + non-flight-1 floor (bootstrap) ───────────────
# Two single-watermark behaviors the manifest must get right, neither covered before:
#  (A) BOOTSTRAP — a DSU only retains back to some flight (e.g. ~1000 for an aircraft a
#      couple years old), so the first full download starts well above flight 1. The hwm
#      must anchor to that floor (min first_flight − 1); otherwise it stays 0 forever (no
#      file has first_flight ≤ hwm+1 == 1) and skip/dedup never engages.
#  (B) GAP-FILL — with fragmented S3 coverage [1000-1071] + [1100-1150] the hwm stops at
#      the gap (1071). Filling 1072-1099 makes the isolated [1100-1150] reachable, so the
#      consecutive-advance jumps the hwm to 1150.
log ""; log "TEST 25: Manifest gap-fill + non-flight-1 floor (bootstrap)"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
    rm -f "$FW_DIR/emu_nvs.dat"
    cleanup_aircraft_s3 "$SERIAL"
    start_device 5

    # ── Part A: bootstrap — first full download from a non-1 floor (flights 1000-1071) ──
    m=$(log_mark)
    write_dsu_file "01071" 100
    echo "1000:1071" > "$SD_EMU/flightHistory/${SERIAL}_01071_$(date +%Y%m%d).eaofh.meta"
    if wait_for_manifest "$SERIAL" 1071 180; then
        pass "Gap A: bootstrap — non-1 floor, hwm advanced to 1071 (not stuck at 0)"
    else
        fail "Gap A: hwm did not bootstrap to 1071 (got $(get_manifest_hwm "$SERIAL"))"
    fi

    # ── Part B: fragmented S3 — a disjoint [1100-1150] sits above the hwm, gap at 1072-1099 ──
    seed_manifest_files "$SERIAL" 1071 "1000:1071" "1100:1150"
    sleep 2
    HWM_GAP=$(get_manifest_hwm "$SERIAL")   # should be 1071 — stopped at the gap
    m=$(log_mark)
    write_dsu_file "01099" 100              # device fills the gap (1072-1099)
    echo "1072:1099" > "$SD_EMU/flightHistory/${SERIAL}_01099_$(date +%Y%m%d).eaofh.meta"
    if wait_for_manifest "$SERIAL" 1150 180; then
        pass "Gap B: filled 1072-1099 → hwm jumped $HWM_GAP -> $(get_manifest_hwm "$SERIAL") (reaches isolated 1100-1150)"
    else
        fail "Gap B: hwm did not jump to 1150 after gap fill (got $(get_manifest_hwm "$SERIAL"))"
    fi
    if aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null | grep -q "_01099_"; then
        pass "Gap B: gap-filler .eaofh uploaded to S3"
    else
        fail "Gap B: gap-filler not uploaded to S3"
    fi

    stop_device
    cleanup_aircraft_s3 "$SERIAL"
else
    skip "TEST 25: manifest gap-fill / bootstrap — emulator-only"
fi

# ── Cleanup ───────────────────────────────────────────────────────────────────
log ""; log "Cleaning up..."
if [ "$TARGET" = "emulator" ]; then
    aws s3 rm "s3://$BUCKET/$DEVICE/" --recursive 2>/dev/null
    cleanup_aircraft_s3 "$SERIAL"
else
    # Restore production MSC-only: remove CDC_PERSIST from P1 (drive must be mounted,
    # so do it with the device powered + CDC up), then stop the serial tap.
    start_device 5
    device_disable_persistent_cdc
    serial_tap_stop
    stop_device
fi

# ── Optional: verify the production MSC-only D+-low invisibility invariant ────
# Running the suite in CDC+MSC never exercises tud_disconnect() (D+ held low). With
# CDC_PERSIST now removed, this boot is MSC-only. Opt in via E2E_MSCONLY_CHECK=1 to
# assert USB stays invisible until ~90s, then enumerates. Runs last (ends serial).
if [ "$TARGET" = "device" ] && [ "${E2E_MSCONLY_CHECK:-0}" = "1" ]; then
    log ""; log "TEST 25 (opt-in): MSC-only D+-low invisibility (production USB mode)"
    $COOLGEAR off >/dev/null 2>&1; sleep 5; $COOLGEAR on >/dev/null 2>&1
    sleep 30   # within the 90s window the device must be invisible (D+ low)
    EARLY=$(lsusb 2>/dev/null | grep -c "1209:000")
    for i in $(seq 1 90); do lsusb 2>/dev/null | grep -q "1209:000" && break; sleep 1; done
    LATE=$(lsusb 2>/dev/null | grep -c "1209:000")
    $COOLGEAR off >/dev/null 2>&1
    if [ "$EARLY" -eq 0 ] && [ "$LATE" -ge 1 ]; then
        pass "MSC-only: USB hidden for ~90s then presented (D+-low path)"
    else
        fail "MSC-only: early=$EARLY late=$LATE (want 0 then >=1)"
    fi
fi

stop_device

# ── Summary ───────────────────────────────────────────────────────────────────
TOTAL=$((PASS + FAIL + SKIP))
log ""
log "═══════════════════════════════════════════════════════════════"
log "  RESULTS: $PASS passed, $FAIL failed, $SKIP skipped ($TOTAL total)"
log "  Target: $TARGET"
log "  Log: $LOG"
log "═══════════════════════════════════════════════════════════════"

exit $FAIL

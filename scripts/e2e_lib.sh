#!/bin/bash
# AirBridge E2E shared library — config + harness helper functions.
# Sourced by e2e_unified.sh and flight_cycle_test.sh. The caller MUST set
# TARGET (device|emulator) before sourcing (config/helpers branch on it).
# Definitions only — sourcing this runs nothing.

: "${TARGET:=emulator}"   # default if caller did not set it


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

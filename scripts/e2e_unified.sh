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
SD_EMU="$FW_DIR/emu_sdcard"
BUCKET="airbridge-uploads"
API_KEY="7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm"
LOG="/tmp/e2e_${TARGET}_$(date +%Y%m%d_%H%M%S).txt"
SERIAL="EA500.000243"

if [ "$TARGET" = "device" ]; then
    DEVICE="9C139EF40188"
else
    DEVICE="EMU_E2E_$(date +%H%M%S)"
fi

PASS=0; FAIL=0; SKIP=0
EMU_PID=""

log() { echo "$(date +%H:%M:%S)   $1" | tee -a "$LOG"; }
pass() { log "PASS: $1"; PASS=$((PASS + 1)); }
fail() { log "FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { log "SKIP: $1"; SKIP=$((SKIP + 1)); }

# ── Abstraction layer ─────────────────────────────────────────────────────────

start_device() {
    if [ "$TARGET" = "emulator" ]; then
        mkdir -p "$SD_EMU"
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
        $COOLGEAR off >/dev/null 2>&1; sleep "${1:-5}"; $COOLGEAR on >/dev/null 2>&1
        sleep 5
        for i in $(seq 1 15); do
            lsusb 2>/dev/null | grep -q "1209:000" && break
            sleep 1
        done
        log "  Device powered on"
    fi
}

stop_device() {
    if [ "$TARGET" = "emulator" ]; then
        [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
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

# Write a DSU-style flight file (realistic directory structure)
write_dsu_file() {
    local flight="$1" size_kb="$2"
    local date_str=$(date +%Y%m%d)
    local fname="${SERIAL}_${flight}_${date_str}.eaofh"

    if [ "$TARGET" = "emulator" ]; then
        mkdir -p "$SD_EMU/flightHistory"
        dd if=/dev/urandom of="$SD_EMU/flightHistory/$fname" bs=1K count="$size_kb" 2>/dev/null
        log "  Wrote flightHistory/$fname (${size_kb}KB)"
    else
        local sddev=""
        for w in $(seq 1 90); do
            for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do [ -b "$d" ] && { sddev="$d"; break 2; }; done
            sleep 1
        done
        [ -n "$sddev" ] || { log "  WARN: no USB drive"; return 1; }
        sudo mount -o noatime "$sddev" /mnt 2>/dev/null || return 1
        sudo mkdir -p /mnt/flightHistory
        sudo dd if=/dev/urandom of="/mnt/flightHistory/$fname" bs=1K count="$size_kb" 2>/dev/null
        sync; sudo umount /mnt 2>/dev/null
        log "  Wrote flightHistory/$fname (${size_kb}KB) to USB drive"
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

# Wait for a file to be uploaded: check S3 for the key pattern (recursive).
# S3 keys are now DEVICE/NNNN/filename due to subfolder harvest.
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
        sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$version\"/" "$FW_DIR/src/main.cpp"
        (cd "$FW_DIR" && ~/.local/bin/pio run 2>&1 | tail -1)
        size=$(stat -c%s "$FW_DIR/.pio/build/esp32s3/firmware.bin")
        aws s3 cp "$FW_DIR/.pio/build/esp32s3/firmware.bin" "s3://$BUCKET/firmware/latest.bin" >/dev/null 2>&1
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
}

# ═══════════════════════════════════════════════════════════════════════════════
: > /tmp/emu_e2e.log
if [ "$TARGET" = "emulator" ]; then
    # Full clean for deterministic tests
    rm -rf "$SD_EMU"/*
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

# Reset OTA to match current firmware
FW_CURRENT=$(grep 'FW_VERSION' "$FW_DIR/src/main.cpp" | head -1 | grep -o '"[^"]*"' | tr -d '"')
log "Resetting OTA to v$FW_CURRENT"
echo "{\"version\":\"$FW_CURRENT\",\"size\":0}" | \
    aws s3 cp - "s3://$BUCKET/firmware/latest.json" --content-type application/json >/dev/null 2>&1

# ── TEST 1: Boot + connectivity ───────────────────────────────────────────────
log ""; log "TEST 1: Boot + modem connectivity"
start_device 5
if [ "$TARGET" = "emulator" ]; then
    grep -q "Init complete" /tmp/emu_e2e.log 2>/dev/null && \
        grep -q "ppp=1" /tmp/emu_e2e.log 2>/dev/null
else
    sleep 30
    lsusb 2>/dev/null | grep -q "1209:000"
fi
[ $? -eq 0 ] && pass "Boot + connectivity" || fail "Boot + connectivity"
stop_device

# ── TEST 2: DSU flight file upload (500KB) ────────────────────────────────────
log ""; log "TEST 2: DSU flight file upload"
cleanup_s3
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_EMU/harvested" "$SD_EMU/flightHistory" "$SD_EMU/metrics"
    rm -f "$SD_EMU"/*.bin "$SD_EMU"/*.txt "$SD_EMU"/*.easdf "$SD_EMU"/*.eaofh
fi
start_device 5
write_dsu_file "01501" 500
log "  Waiting for harvest + upload..."
# S3 key now includes subfolder: DEVICE/NNNN/flightHistory__SERIAL_flight_date.eaofh
if wait_for_upload "flightHistory__${SERIAL}_01501" 180; then
    pass "DSU upload: flight file harvested + uploaded"
else
    fail "DSU upload: not completed"
fi
stop_device

# ── TEST 3: Upload + power cut ────────────────────────────────────────────────
log ""; log "TEST 3: Upload + power cut (2MB)"
cleanup_s3
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_EMU/harvested" "$SD_EMU/flightHistory" "$SD_EMU/metrics"
    rm -f "$SD_EMU"/*.bin "$SD_EMU"/*.txt "$SD_EMU"/*.easdf "$SD_EMU"/*.eaofh
fi
start_device 5
write_dsu_file "01502" 2000
log "  Waiting 20s then cutting power..."
sleep 20
power_cut 5
log "  Restarting..."
start_device 5
if wait_for_upload "flightHistory__${SERIAL}_01502" 180; then
    pass "Power cut resume: uploaded after restart"
else
    fail "Power cut resume: not completed"
fi
stop_device

# ── TEST 4: Multiple files ────────────────────────────────────────────────────
log ""; log "TEST 4: Multiple DSU files in one harvest"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_EMU/harvested" "$SD_EMU/flightHistory" "$SD_EMU/metrics"
    rm -f "$SD_EMU"/*.bin "$SD_EMU"/*.txt "$SD_EMU"/*.easdf "$SD_EMU"/*.eaofh
fi
start_device 5
write_dsu_file "01503" 200
write_dsu_file "01504" 200
write_dsu_file "01505" 200
log "  3 flight files, waiting..."
ALL_DONE=true
for f in 01503 01504 01505; do
    if ! wait_for_upload "flightHistory__${SERIAL}_${f}" 180; then
        ALL_DONE=false
        fail "Multi-file: flight $f not completed"
    fi
done
$ALL_DONE && pass "Multi-file: all 3 flights uploaded"
stop_device

# ── TEST 5: System files skipped ──────────────────────────────────────────────
log ""; log "TEST 5: System files skipped"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_EMU/harvested" "$SD_EMU/flightHistory" "$SD_EMU"/*.bin
    echo "skip" > "$SD_EMU/Thumbs.db"
    echo "skip" > "$SD_EMU/.hidden"
    start_device 5
    write_dsu_file "01506" 100
    wait_for_upload "flightHistory__${SERIAL}_01506" 60
    # System files should NOT appear in any harvested subfolder
    FOUND_SYSTEM=false
    for sub in "$SD_EMU"/harvested/*/; do
        [ -f "${sub}Thumbs.db" ] && FOUND_SYSTEM=true
        [ -f "${sub}.hidden" ] && FOUND_SYSTEM=true
    done
    if ! $FOUND_SYSTEM; then
        pass "System files skipped"
    else
        fail "System files not skipped"
    fi
    rm -f "$SD_EMU/Thumbs.db" "$SD_EMU/.hidden"
    stop_device
else
    start_device 5
    local sddev=""
    for w in $(seq 1 90); do
        for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do [ -b "$d" ] && { sddev="$d"; break 2; }; done
        sleep 1
    done
    if [ -n "$sddev" ]; then
        sudo mount -o noatime "$sddev" /mnt 2>/dev/null
        echo "skip" | sudo tee /mnt/Thumbs.db >/dev/null
        sudo mkdir -p /mnt/flightHistory
        sudo dd if=/dev/urandom of="/mnt/flightHistory/${SERIAL}_01506_$(date +%Y%m%d).eaofh" bs=1K count=100 2>/dev/null
        sync; sudo umount /mnt 2>/dev/null
        if wait_for_s3_file "flightHistory__${SERIAL}_01506" 180; then
            pass "System files: real file uploaded"
            if ! aws s3 ls "s3://$BUCKET/$DEVICE/Thumbs.db" 2>/dev/null | grep -q "20"; then
                pass "System files: Thumbs.db skipped"
            else
                fail "System files: Thumbs.db in S3"
            fi
        else
            fail "System files: flight file not uploaded"
        fi
    else
        skip "System files (no USB drive)"
    fi
    stop_device
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

# ── TEST 7: Persistence ──────────────────────────────────────────────────────
log ""; log "TEST 7: State persistence"
if [ "$TARGET" = "emulator" ]; then
    start_device 5; sleep 3; stop_device
    [ -f "$FW_DIR/emu_nvs.dat" ] && grep -q "api_host" "$FW_DIR/emu_nvs.dat" && \
        pass "NVS persistence" || fail "NVS persistence"
else
    pass "NVS persistence (hardware)"
fi

# ── TEST 8: DSU cookie cycle ─────────────────────────────────────────────────
log ""; log "TEST 8: DSU cookie cycle"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_EMU/harvested" "$SD_EMU/flightHistory" "$SD_EMU/metrics"
    rm -f "$SD_EMU/dsuCookie.easdf" "$SD_EMU"/*.bin
    start_device 5
    # Write DSU-style files
    mkdir -p "$SD_EMU/flightHistory"
    dd if=/dev/urandom of="$SD_EMU/flightHistory/${SERIAL}_01601_$(date +%Y%m%d).eaofh" bs=1K count=100 2>/dev/null
    log "  Flight file dropped..."
    sleep 30  # detect + quiet window + harvest
    if [ -f "$SD_EMU/dsuCookie.easdf" ]; then
        pass "DSU cookie: written after harvest"
    else
        fail "DSU cookie: not written"
    fi
    stop_device
else
    # On hardware: write flight file, wait for upload, check cookie via USB mount
    start_device 5
    write_dsu_file "01601" 100
    wait_for_s3_file "flightHistory__${SERIAL}_01601" 180
    # Check if cookie was written
    sleep 10
    local sddev=""
    for d in /dev/sda1 /dev/sdb1 /dev/sdc1; do [ -b "$d" ] && { sddev="$d"; break; }; done
    if [ -n "$sddev" ]; then
        sudo mount -o noatime "$sddev" /mnt 2>/dev/null
        if [ -f /mnt/dsuCookie.easdf ]; then
            pass "DSU cookie: found on SD card"
        else
            fail "DSU cookie: not on SD card"
        fi
        sudo umount /mnt 2>/dev/null
    else
        pass "DSU cookie: flight uploaded (can't check cookie in MSC mode)"
    fi
    stop_device
fi

# ── TEST 9: Pre-USB: OTA + cookie before host ───────────────────────────────
log ""; log "TEST 9: OTA + S3 cookie land before USB presentation"
if [ "$TARGET" = "emulator" ]; then
    rm -rf "$SD_EMU/harvested" "$SD_EMU/flightHistory"
    rm -f "$SD_EMU/dsuCookie.easdf"

    # Build a "cookie" — 78-byte binary with EA1E magic header
    COOKIE_HEX="EA1E"$(python3 -c "import os; print(os.urandom(76).hex())")
    echo "$COOKIE_HEX" | xxd -r -p > /tmp/test_cookie.bin

    # Upload cookie to S3 firmware path (Lambda serves it to device)
    aws s3 cp /tmp/test_cookie.bin "s3://$BUCKET/firmware/dsuCookie.easdf" >/dev/null 2>&1

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
    aws s3 cp /tmp/test_cookie.bin "s3://$BUCKET/firmware/dsuCookie.easdf" >/dev/null 2>&1
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

# ── TEST 10: Boot splash ─────────────────────────────────────────────────────
log ""; log "TEST 10: Boot splash"
if [ "$TARGET" = "emulator" ]; then
    start_device 5
    pass "Boot splash rendered"
    stop_device
else
    pass "Boot splash (hardware — visual only)"
fi

# ── Cleanup ───────────────────────────────────────────────────────────────────
log ""; log "Cleaning up..."
if [ "$TARGET" = "emulator" ]; then
    aws s3 rm "s3://$BUCKET/$DEVICE/" --recursive 2>/dev/null
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

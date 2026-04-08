#!/bin/bash
# AirBridge E2E Test Suite
# Tests OTA, file upload, and resilience to power cuts
set -u

COOLGEAR="python3 /home/cedric/USBCellular/scripts/coolgear.py"
FW_DIR="/home/cedric/USBCellular/esp32"
BUCKET="airbridge-uploads"
DEVICE="9C139EF40188"
API_KEY="7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm"
API_HOST="disw6oxjed.execute-api.us-west-2.amazonaws.com"
LOG="/tmp/e2e_results_$(date +%Y%m%d_%H%M%S).txt"

PASS=0
FAIL=0
SKIP=0

log() { echo "$1" | tee -a "$LOG"; }
pass() { log "  PASS: $1"; ((PASS++)); }
fail() { log "  FAIL: $1"; ((FAIL++)); }
skip() { log "  SKIP: $1"; ((SKIP++)); }

power_cycle() {
    local off_time=${1:-5}
    $COOLGEAR off >/dev/null 2>&1
    sleep "$off_time"
    $COOLGEAR on >/dev/null 2>&1
}

wait_for_usb() {
    for i in $(seq 1 15); do
        if lsusb 2>/dev/null | grep -q "1209:000"; then return 0; fi
        sleep 1
    done
    return 1
}

wait_for_lambda() {
    local min_events=$1
    local timeout=$2
    local start_ts=$(python3 -c "import time; print(int((time.time()-5)*1000))")
    for i in $(seq 1 $((timeout / 5))); do
        sleep 5
        local count=$(aws logs filter-log-events --log-group-name /aws/lambda/airbridge-presign \
            --start-time "$start_ts" --limit 30 \
            --query "length(events)" --output text 2>&1)
        if [ "$count" -ge "$min_events" ] 2>/dev/null; then return 0; fi
    done
    return 1
}

get_fw_version() {
    # Try serial first (CDC mode)
    if [ -e /dev/ttyACM0 ]; then
        python3 -c "
import serial, time
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'STATUS\r\n')
time.sleep(2)
data = s.read(2048).decode(errors='replace')
for l in data.split('\n'):
    if 'fw=' in l:
        import re
        m = re.search(r'fw=(\S+)', l)
        if m: print(m.group(1))
s.close()
" 2>/dev/null
        return
    fi
    # Fallback: check latest S3 log
    local latest=$(aws s3 ls s3://$BUCKET/$DEVICE/logs/ --recursive 2>&1 | sort -k1,2 | tail -1 | awk '{print $4}')
    if [ -n "$latest" ]; then
        aws s3 cp "s3://$BUCKET/$latest" - 2>&1 | grep -oP 'fw=\K\S+' | head -1
    fi
}

deploy_ota() {
    local version=$1
    sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$version\"/" "$FW_DIR/src/main.cpp"
    (cd "$FW_DIR" && ~/.local/bin/pio run 2>&1 | tail -1)
    local size=$(stat -c%s "$FW_DIR/.pio/build/esp32s3/firmware.bin")
    aws s3 cp "$FW_DIR/.pio/build/esp32s3/firmware.bin" "s3://$BUCKET/firmware/latest.bin" >/dev/null 2>&1
    echo "{\"version\":\"$version\",\"size\":$size}" | \
        aws s3 cp - "s3://$BUCKET/firmware/latest.json" --content-type application/json >/dev/null 2>&1
}

write_test_file() {
    local name=$1
    local size_mb=$2
    # Clean done marker + any leftover copies
    sudo mount -o noatime /dev/sda1 /mnt 2>/dev/null
    sudo rm -f "/mnt/harvested/.done__$name" "/mnt/harvested/$name" "/mnt/$name" 2>/dev/null
    # Also clean all test-related done markers
    sudo rm -f /mnt/harvested/.done__test_* 2>/dev/null
    sudo dd if=/dev/urandom of="/mnt/$name" bs=1M count="$size_mb" 2>/dev/null
    sync
    sudo umount /mnt 2>/dev/null
}

wait_for_upload() {
    local key=$1
    local timeout=$2
    for i in $(seq 1 $((timeout / 10))); do
        sleep 10
        local found=$(aws s3 ls "s3://$BUCKET/$DEVICE/$key" 2>&1 | grep "2026-")
        if [ -n "$found" ]; then echo "$found"; return 0; fi
    done
    return 1
}

cleanup() {
    # Abort stale multiparts
    for uid in $(aws s3api list-multipart-uploads --bucket $BUCKET \
        --query "Uploads[].UploadId" --output text 2>/dev/null); do
        local key=$(aws s3api list-multipart-uploads --bucket $BUCKET \
            --query "Uploads[?UploadId=='$uid'].Key" --output text)
        aws s3api abort-multipart-upload --bucket $BUCKET --key "$key" --upload-id "$uid" 2>/dev/null
    done
}

# ═══════════════════════════════════════════════════════════════
log "═══════════════════════════════════════════════════════════════"
log "  AirBridge E2E Test Suite — $(date)"
log "═══════════════════════════════════════════════════════════════"
log ""

# ── TEST 1: Normal OTA ─────────────────────────────────────────
log "TEST 1: Normal OTA update"
log "  Deploy v3.1.0 to S3, power cycle, verify version"
cleanup
deploy_ota "3.1.0"
# Set source back to 3.0.0 (the flashed version)
sed -i 's/#define FW_VERSION "[^"]*"/#define FW_VERSION "3.0.0"/' "$FW_DIR/src/main.cpp"

power_cycle 30  # long off for modem drain
sleep 5
wait_for_usb || { fail "USB not enumerated"; }

# Wait for OTA + auto-reboot (up to 3 min)
log "  Waiting for OTA download + auto-reboot..."
V=""
for i in $(seq 1 30); do
    sleep 10
    # Check if device rebooted (USB reconnect)
    if ! lsusb 2>/dev/null | grep -q "1209:000"; then
        log "  Device rebooting at $((i*10))s..."
        sleep 10
        wait_for_usb
    fi
    V=$(get_fw_version)
    if [ "$V" = "3.1.0" ]; then break; fi
done

if [ "$V" = "3.1.0" ]; then
    pass "OTA: v3.0.0 → v3.1.0"
else
    fail "OTA: expected v3.1.0, got '$V'"
fi

# ── TEST 2: OTA with power cut ─────────────────────────────────
log ""
log "TEST 2: OTA with power cut mid-download"
log "  Deploy v3.2.0, power cycle, cut power at ~30s, power back on"
deploy_ota "3.2.0"
sed -i 's/#define FW_VERSION "[^"]*"/#define FW_VERSION "3.1.0"/' "$FW_DIR/src/main.cpp"

power_cycle 30
sleep 5
wait_for_usb

# Wait for OTA to start downloading (~40s after boot)
log "  Waiting for OTA download to start..."
sleep 40

# Cut power mid-download
log "  Cutting power mid-download..."
$COOLGEAR off >/dev/null 2>&1
sleep 10

# Power back on — should retry OTA
log "  Powering back on..."
$COOLGEAR on >/dev/null 2>&1
sleep 5
wait_for_usb

# Wait for OTA to complete on retry + auto-reboot
log "  Waiting for OTA retry + reboot..."
V=""
for i in $(seq 1 24); do
    sleep 10
    if ! lsusb 2>/dev/null | grep -q "1209:000"; then
        log "  Device rebooting at $((i*10))s..."
        sleep 10
        wait_for_usb
    fi
    V=$(get_fw_version)
    if [ "$V" = "3.2.0" ]; then break; fi
done

if [ "$V" = "3.2.0" ]; then
    pass "OTA interrupted + resumed: v3.1.0 → v3.2.0"
else
    fail "OTA interrupted: expected v3.2.0, got '$V'"
fi

# ── TEST 3: Normal file upload ──────────────────────────────────
log ""
log "TEST 3: Normal 10MB file upload"
cleanup
aws s3 rm "s3://$BUCKET/$DEVICE/test_upload.bin" 2>/dev/null
# Set S3 version to match device so OTA doesn't interfere
deploy_ota "3.2.0"
sed -i 's/#define FW_VERSION "[^"]*"/#define FW_VERSION "3.2.0"/' "$FW_DIR/src/main.cpp"

write_test_file "test_upload.bin" 10
power_cycle 30
sleep 5
wait_for_usb

log "  Waiting for harvest + upload (up to 10 min)..."
RESULT=$(wait_for_upload "test_upload.bin" 600)
if [ -n "$RESULT" ]; then
    pass "Upload: 10MB file — $RESULT"
else
    fail "Upload: test_upload.bin not found on S3 after 10 min"
fi

# ── TEST 4: Upload with power cut ───────────────────────────────
log ""
log "TEST 4: Upload with power cut mid-transfer"
cleanup
aws s3 rm "s3://$BUCKET/$DEVICE/test_resume.bin" 2>/dev/null

write_test_file "test_resume.bin" 10
power_cycle 30
sleep 5
wait_for_usb

# Wait for multipart to start
log "  Waiting for upload to start..."
MP_FOUND=false
for i in $(seq 1 30); do
    sleep 10
    MP=$(aws s3api list-multipart-uploads --bucket $BUCKET \
        --query "Uploads[?contains(Key,'test_resume')].UploadId" --output text 2>&1)
    if [ -n "$MP" ] && [ "$MP" != "None" ]; then
        PARTS=$(aws s3api list-parts --bucket $BUCKET \
            --key "$DEVICE/test_resume.bin" --upload-id "$MP" \
            --query "length(Parts || \`[]\`)" --output text 2>&1)
        log "  Upload in progress: $PARTS part(s) at $((i*10))s"
        if [ "$PARTS" -ge 1 ] 2>/dev/null; then
            MP_FOUND=true
            break
        fi
    fi
done

if $MP_FOUND; then
    # Cut power mid-upload
    log "  Cutting power mid-upload..."
    $COOLGEAR off >/dev/null 2>&1
    sleep 10

    log "  Powering back on..."
    $COOLGEAR on >/dev/null 2>&1
    sleep 5
    wait_for_usb

    log "  Waiting for upload resume (up to 10 min)..."
    RESULT=$(wait_for_upload "test_resume.bin" 600)
    if [ -n "$RESULT" ]; then
        pass "Upload resumed after power cut — $RESULT"
    else
        fail "Upload resume: test_resume.bin not found on S3"
    fi
else
    fail "Upload never started (multipart not initiated)"
fi

# ── TEST 5: Combined OTA + upload ──────────────────────────────
log ""
log "TEST 5: Simultaneous OTA + file upload"
cleanup
aws s3 rm "s3://$BUCKET/$DEVICE/test_combo.bin" 2>/dev/null
deploy_ota "3.3.0"
sed -i 's/#define FW_VERSION "[^"]*"/#define FW_VERSION "3.2.0"/' "$FW_DIR/src/main.cpp"

write_test_file "test_combo.bin" 10
power_cycle 30
sleep 5
wait_for_usb

log "  Waiting for OTA + upload (up to 20 min)..."
OTA_OK=false
UPLOAD_OK=false
for i in $(seq 1 120); do
    sleep 10

    # Check OTA (device reboots)
    if ! $OTA_OK; then
        if ! lsusb 2>/dev/null | grep -q "1209:000"; then
            log "  $((i*10))s: Device rebooting (OTA applied)..."
            sleep 10
            wait_for_usb
        fi
        V=$(get_fw_version)
        if [ "$V" = "3.3.0" ]; then
            log "  OTA complete: v3.3.0"
            OTA_OK=true
        fi
    fi

    # Check upload
    if ! $UPLOAD_OK; then
        FOUND=$(aws s3 ls "s3://$BUCKET/$DEVICE/test_combo.bin" 2>&1 | grep "2026-")
        if [ -n "$FOUND" ]; then
            log "  Upload complete: $FOUND"
            UPLOAD_OK=true
        fi
    fi

    if $OTA_OK && $UPLOAD_OK; then break; fi
done

if $OTA_OK; then pass "Combined: OTA v3.2.0 → v3.3.0"
else fail "Combined: OTA failed (got $(get_fw_version))"; fi

if $UPLOAD_OK; then pass "Combined: 10MB upload"
else fail "Combined: upload not completed"; fi

# ── TEST 6: Combined OTA + upload with power cut ───────────────
log ""
log "TEST 6: OTA + upload with power cut"
cleanup
aws s3 rm "s3://$BUCKET/$DEVICE/test_chaos.bin" 2>/dev/null
deploy_ota "3.4.0"
sed -i 's/#define FW_VERSION "[^"]*"/#define FW_VERSION "3.3.0"/' "$FW_DIR/src/main.cpp"

write_test_file "test_chaos.bin" 10
power_cycle 30
sleep 5
wait_for_usb

# Wait ~45s for things to start, then cut power
log "  Waiting 45s then cutting power..."
sleep 45
$COOLGEAR off >/dev/null 2>&1
sleep 10

log "  Powering back on..."
$COOLGEAR on >/dev/null 2>&1
sleep 5
wait_for_usb

log "  Waiting for recovery (up to 20 min)..."
OTA_OK=false
UPLOAD_OK=false
for i in $(seq 1 120); do
    sleep 10

    if ! $OTA_OK; then
        if ! lsusb 2>/dev/null | grep -q "1209:000"; then
            sleep 10; wait_for_usb
        fi
        V=$(get_fw_version)
        if [ "$V" = "3.4.0" ]; then OTA_OK=true; log "  OTA recovered: v3.4.0"; fi
    fi

    if ! $UPLOAD_OK; then
        FOUND=$(aws s3 ls "s3://$BUCKET/$DEVICE/test_chaos.bin" 2>&1 | grep "2026-")
        if [ -n "$FOUND" ]; then UPLOAD_OK=true; log "  Upload recovered: $FOUND"; fi
    fi

    if $OTA_OK && $UPLOAD_OK; then break; fi
done

if $OTA_OK; then pass "Chaos: OTA recovered"
else fail "Chaos: OTA failed (got $(get_fw_version))"; fi

if $UPLOAD_OK; then pass "Chaos: upload recovered"
else fail "Chaos: upload not recovered"; fi

# ── TEST 7: OTA waits for host USB writes ───────────────────────
log ""
log "TEST 7: OTA download while host is writing (should wait for idle)"
cleanup
deploy_ota "3.5.0"
sed -i 's/#define FW_VERSION "[^"]*"/#define FW_VERSION "3.4.0"/' "$FW_DIR/src/main.cpp"

power_cycle 30
sleep 5
wait_for_usb

# Start writing a file to SD WHILE OTA is downloading
# The device should: download OTA, then wait for our writes to stop, then reboot
log "  Waiting for PPP (~30s)..."
sleep 25

# Write slowly to SD to simulate ongoing host transfer
log "  Writing to SD during OTA window..."
if [ -b /dev/sda1 ]; then
    sudo mount -o noatime /dev/sda1 /mnt 2>/dev/null
    # Write in small chunks with delays to simulate avionics writing over ~60s
    for chunk in $(seq 1 6); do
        sudo dd if=/dev/urandom of="/mnt/test_ota_write_$chunk.bin" bs=100K count=1 2>/dev/null
        sync
        sleep 10
    done
    sudo umount /mnt 2>/dev/null
    log "  Host writes finished. Device should reboot in ~15s..."
fi

# Wait for OTA reboot (should happen ~15s after last write)
V=""
for i in $(seq 1 30); do
    sleep 10
    if ! lsusb 2>/dev/null | grep -q "1209:000"; then
        log "  $((i*10))s: Device rebooting..."
        sleep 10
        wait_for_usb
    fi
    V=$(get_fw_version)
    if [ "$V" = "3.5.0" ]; then break; fi
done

if [ "$V" = "3.5.0" ]; then
    pass "OTA waited for host writes: v3.4.0 → v3.5.0"
else
    fail "OTA+write: expected v3.5.0, got '$V'"
fi

# ═══════════════════════════════════════════════════════════════
log ""
log "═══════════════════════════════════════════════════════════════"
log "  RESULTS: $PASS passed, $FAIL failed, $SKIP skipped"
log "  Log: $LOG"
log "═══════════════════════════════════════════════════════════════"

# Reset source version
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"3.4.0\"/" "$FW_DIR/src/main.cpp"

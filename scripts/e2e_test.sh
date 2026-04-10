#!/bin/bash
# AirBridge E2E Test Suite — tightened timing + progress output
set -u

COOLGEAR="python3 /home/cedric/USBCellular/scripts/coolgear.py"
FW_DIR="/home/cedric/USBCellular/esp32"
BUCKET="airbridge-uploads"
DEVICE="9C139EF40188"
API_KEY="7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm"
API_HOST="disw6oxjed.execute-api.us-west-2.amazonaws.com"
LOG="/tmp/e2e_results_$(date +%Y%m%d_%H%M%S).txt"

PASS=0; FAIL=0; SKIP=0

log() { echo "$(date +%H:%M:%S) $1" | tee -a "$LOG"; }
pass() { log "  PASS: $1"; ((PASS++)); }
fail() { log "  FAIL: $1"; ((FAIL++)); }

power_cycle() {
    $COOLGEAR off >/dev/null 2>&1; sleep "${1:-5}"; $COOLGEAR on >/dev/null 2>&1
}

wait_for_usb() {
    for i in $(seq 1 15); do
        if lsusb 2>/dev/null | grep -q "1209:000"; then return 0; fi
        sleep 1
    done; return 1
}

get_fw_version() {
    if [ -e /dev/ttyACM0 ]; then
        python3 -c "
import serial, time, re
s = serial.Serial('/dev/ttyACM0', 115200, timeout=3)
s.write(b'STATUS\r\n')
time.sleep(2)
data = s.read(4096).decode(errors='replace')
m = re.search(r'fw=([0-9]+\.[0-9]+\.[0-9]+)', data)
if m: print(m.group(1))
s.close()
" 2>/dev/null
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
    local name=$1 size_mb=$2
    for w in $(seq 1 10); do [ -b /dev/sda1 ] && break; sleep 1; done
    [ -b /dev/sda1 ] || { log "  WARN: /dev/sda1 not found"; return 1; }
    sudo mount -o noatime /dev/sda1 /mnt 2>/dev/null || { log "  WARN: mount failed"; return 1; }
    sudo rm -f /mnt/harvested/.done__test_* /mnt/harvested/test_*.bin /mnt/test_*.bin 2>/dev/null
    sudo dd if=/dev/urandom of="/mnt/$name" bs=1M count="$size_mb" 2>/dev/null
    sync; sudo umount /mnt 2>/dev/null
    log "  Wrote $name (${size_mb}MB)"
}

# Wait for OTA reboot: polls version every 5s, detects USB disconnect/reconnect
wait_for_ota() {
    local expected=$1 timeout=${2:-180}
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t + 5))
        if ! lsusb 2>/dev/null | grep -q "1209:000"; then
            log "  ${t}s: device rebooting..." >&2
            sleep 10; t=$((t + 10)); wait_for_usb; sleep 5; t=$((t + 5))
        fi
        local v=$(get_fw_version | tr -d '[:space:]')
        [ "$v" = "$expected" ] && { echo "$v"; return 0; }
        [ $((t % 30)) -eq 0 ] && [ -n "$v" ] && log "  ${t}s: fw=$v (waiting for $expected)" >&2
    done
    get_fw_version | tr -d '[:space:]'; return 1
}

# Wait for file to appear on S3
wait_for_s3_file() {
    local key=$1 timeout=${2:-300}
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 10; t=$((t + 10))
        local found=$(aws s3 ls "s3://$BUCKET/$DEVICE/$key" 2>&1 | grep "2026-")
        if [ -n "$found" ]; then echo "$found"; return 0; fi
        local mp=$(aws s3api list-multipart-uploads --bucket $BUCKET \
            --query "Uploads[?contains(Key,'$key')].UploadId" --output text 2>&1)
        if [ -n "$mp" ] && [ "$mp" != "None" ]; then
            local parts=$(aws s3api list-parts --bucket $BUCKET \
                --key "$DEVICE/$key" --upload-id "$mp" \
                --query "length(Parts || \`[]\`)" --output text 2>&1)
            log "  ${t}s: uploading $key — $parts part(s)" >&2
        else
            [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for $key..." >&2
        fi
    done
    return 1
}

cleanup() {
    for uid in $(aws s3api list-multipart-uploads --bucket $BUCKET \
        --query "Uploads[].UploadId" --output text 2>/dev/null); do
        local key=$(aws s3api list-multipart-uploads --bucket $BUCKET \
            --query "Uploads[?UploadId=='$uid'].Key" --output text)
        aws s3api abort-multipart-upload --bucket $BUCKET --key "$key" --upload-id "$uid" 2>/dev/null
    done
}

# ═══════════════════════════════════════════════════════════════
log "═══════════════════════════════════════════════════════════════"
log "  AirBridge E2E Test Suite"
log "═══════════════════════════════════════════════════════════════"

# Dynamic version numbers
BASE_VER="10.$(date +%H%M)"
V1="${BASE_VER}.1"; V2="${BASE_VER}.2"; V3="${BASE_VER}.3"
V4="${BASE_VER}.4"; V5="${BASE_VER}.5"; VBASE="${BASE_VER}.0"
log "Versions: $VBASE → $V5"

# Flash base version
log "Flashing base firmware v$VBASE..."
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$VBASE\"/" "$FW_DIR/src/main.cpp"
(cd "$FW_DIR" && ~/.local/bin/pio run 2>&1 | tail -1)
python3 -c "import serial; s=serial.Serial('/dev/ttyACM0', 1200); s.close()" 2>/dev/null
sleep 3
(cd "$FW_DIR" && ~/.local/bin/pio run -t upload --upload-port /dev/ttyACM0 2>&1 | tail -1)

# Clean SD
power_cycle 5; sleep 8
if [ -b /dev/sda1 ]; then
    sudo mount -o noatime /dev/sda1 /mnt 2>/dev/null
    sudo rm -f /mnt/harvested/.done__test_* /mnt/test_*.bin /mnt/harvested/test_*.bin 2>/dev/null
    sudo umount /mnt 2>/dev/null
fi

# ── TEST 1: Normal OTA ─────────────────────────────────────────
log ""
log "TEST 1: Normal OTA update ($VBASE → $V1)"
cleanup; deploy_ota "$V1"
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$VBASE\"/" "$FW_DIR/src/main.cpp"
power_cycle 5; sleep 5; wait_for_usb
V=$(wait_for_ota "$V1" 180)
[ "$V" = "$V1" ] && pass "OTA: $VBASE → $V1" || fail "OTA: expected $V1, got '$V'"

# ── TEST 2: OTA with power cut ─────────────────────────────────
log ""
log "TEST 2: OTA + power cut ($V1 → $V2)"
deploy_ota "$V2"
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$V1\"/" "$FW_DIR/src/main.cpp"
power_cycle 5; sleep 5; wait_for_usb
log "  Cutting power at 30s..."
sleep 30; $COOLGEAR off >/dev/null 2>&1; sleep 10
log "  Powering back on..."
$COOLGEAR on >/dev/null 2>&1; sleep 5; wait_for_usb
V=$(wait_for_ota "$V2" 180)
[ "$V" = "$V2" ] && pass "OTA interrupted + resumed: $V1 → $V2" || fail "OTA interrupted: expected $V2, got '$V'"

# ── TEST 3: Normal 10MB upload ──────────────────────────────────
log ""
log "TEST 3: Normal 10MB upload"
cleanup; aws s3 rm "s3://$BUCKET/$DEVICE/test_upload.bin" 2>/dev/null
CUR=$(get_fw_version); deploy_ota "${CUR:-$V2}"
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"${CUR:-$V2}\"/" "$FW_DIR/src/main.cpp"
write_test_file "test_upload.bin" 10
power_cycle 5; sleep 5; wait_for_usb
RESULT=$(wait_for_s3_file "test_upload.bin" 300)
[ -n "$RESULT" ] && pass "Upload: $RESULT" || fail "Upload: not found after 5 min"

# ── TEST 4: Upload with power cut ───────────────────────────────
log ""
log "TEST 4: Upload + power cut"
cleanup; aws s3 rm "s3://$BUCKET/$DEVICE/test_resume.bin" 2>/dev/null
CUR=$(get_fw_version); deploy_ota "${CUR:-$V2}"
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"${CUR:-$V2}\"/" "$FW_DIR/src/main.cpp"
write_test_file "test_resume.bin" 10
power_cycle 5; sleep 5; wait_for_usb
# Wait for upload to start then cut power
MP_CUT=false
for i in $(seq 1 20); do
    sleep 10
    MP=$(aws s3api list-multipart-uploads --bucket $BUCKET \
        --query "Uploads[?contains(Key,'test_resume')].UploadId" --output text 2>&1)
    if [ -n "$MP" ] && [ "$MP" != "None" ]; then
        PARTS=$(aws s3api list-parts --bucket $BUCKET \
            --key "$DEVICE/test_resume.bin" --upload-id "$MP" \
            --query "length(Parts || \`[]\`)" --output text 2>&1)
        log "  $((i*10))s: $PARTS part(s) uploaded"
        if [ "$PARTS" -ge 1 ] 2>/dev/null; then
            log "  Cutting power mid-upload..."
            $COOLGEAR off >/dev/null 2>&1; sleep 10
            log "  Powering back on..."
            $COOLGEAR on >/dev/null 2>&1; sleep 5; wait_for_usb
            MP_CUT=true; break
        fi
    else
        log "  $((i*10))s: waiting for multipart..."
    fi
done
if $MP_CUT; then
    RESULT=$(wait_for_s3_file "test_resume.bin" 300)
    [ -n "$RESULT" ] && pass "Upload resumed: $RESULT" || fail "Upload resume failed"
else
    fail "Upload never started"
fi

# ── TEST 5: Combined OTA + upload ──────────────────────────────
log ""
log "TEST 5: OTA + upload ($V2 → $V3)"
cleanup; aws s3 rm "s3://$BUCKET/$DEVICE/test_combo.bin" 2>/dev/null
deploy_ota "$V3"
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$V2\"/" "$FW_DIR/src/main.cpp"
write_test_file "test_combo.bin" 10
power_cycle 5; sleep 5; wait_for_usb
OTA_OK=false; UPLOAD_OK=false
for i in $(seq 1 120); do
    sleep 10
    if ! $OTA_OK; then
        if ! lsusb 2>/dev/null | grep -q "1209:000"; then
            log "  $((i*10))s: rebooting (OTA)..."
            sleep 15; wait_for_usb; sleep 5
        fi
        local_v=$(get_fw_version)
        [ "$local_v" = "$V3" ] && { OTA_OK=true; log "  OTA: $V3 ✓"; }
    fi
    if ! $UPLOAD_OK; then
        found=$(aws s3 ls "s3://$BUCKET/$DEVICE/test_combo.bin" 2>&1 | grep "2026-")
        [ -n "$found" ] && { UPLOAD_OK=true; log "  Upload: $found ✓"; }
    fi
    $OTA_OK && $UPLOAD_OK && break
    [ $((i % 6)) -eq 0 ] && log "  $((i*10))s: ota=$OTA_OK upload=$UPLOAD_OK"
done
$OTA_OK && pass "Combined OTA: $V2 → $V3" || fail "Combined OTA failed (got $(get_fw_version))"
$UPLOAD_OK && pass "Combined upload" || fail "Combined upload failed"

# ── TEST 6: OTA + upload + power cut ───────────────────────────
log ""
log "TEST 6: Chaos test ($V3 → $V4)"
cleanup; aws s3 rm "s3://$BUCKET/$DEVICE/test_chaos.bin" 2>/dev/null
deploy_ota "$V4"
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$V3\"/" "$FW_DIR/src/main.cpp"
write_test_file "test_chaos.bin" 10
power_cycle 5; sleep 5; wait_for_usb
log "  Cutting power at 45s..."
sleep 45; $COOLGEAR off >/dev/null 2>&1; sleep 10
log "  Powering back on..."
$COOLGEAR on >/dev/null 2>&1; sleep 5; wait_for_usb
OTA_OK=false; UPLOAD_OK=false
for i in $(seq 1 120); do
    sleep 10
    if ! $OTA_OK; then
        if ! lsusb 2>/dev/null | grep -q "1209:000"; then
            sleep 15; wait_for_usb; sleep 5
        fi
        local_v=$(get_fw_version)
        [ "$local_v" = "$V4" ] && { OTA_OK=true; log "  OTA recovered: $V4 ✓"; }
    fi
    if ! $UPLOAD_OK; then
        found=$(aws s3 ls "s3://$BUCKET/$DEVICE/test_chaos.bin" 2>&1 | grep "2026-")
        [ -n "$found" ] && { UPLOAD_OK=true; log "  Upload recovered ✓"; }
    fi
    $OTA_OK && $UPLOAD_OK && break
    [ $((i % 6)) -eq 0 ] && log "  $((i*10))s: ota=$OTA_OK upload=$UPLOAD_OK"
done
$OTA_OK && pass "Chaos: OTA recovered" || fail "Chaos: OTA failed"
$UPLOAD_OK && pass "Chaos: upload recovered" || fail "Chaos: upload failed"

# ── TEST 7: OTA waits for USB writes ───────────────────────────
log ""
log "TEST 7: OTA waits for USB writes ($V4 → $V5)"
cleanup; deploy_ota "$V5"
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$V4\"/" "$FW_DIR/src/main.cpp"
power_cycle 5; sleep 5; wait_for_usb
log "  Writing to SD during OTA window..."
sleep 25
if [ -b /dev/sda1 ]; then
    sudo mount -o noatime /dev/sda1 /mnt 2>/dev/null
    for chunk in $(seq 1 6); do
        sudo dd if=/dev/urandom of="/mnt/test_ota_write_$chunk.bin" bs=100K count=1 2>/dev/null
        sync; sleep 10
    done
    sudo umount /mnt 2>/dev/null
    log "  Writes done. OTA should reboot in ~15s..."
fi
V=$(wait_for_ota "$V5" 180)
[ "$V" = "$V5" ] && pass "OTA waited for writes: $V4 → $V5" || fail "OTA+write: expected $V5, got '$V'"

# ═══════════════════════════════════════════════════════════════
log ""
log "═══════════════════════════════════════════════════════════════"
log "  RESULTS: $PASS passed, $FAIL failed, $SKIP skipped"
log "  Log: $LOG"
log "═══════════════════════════════════════════════════════════════"

# Reset source version
sed -i "s/#define FW_VERSION \"[^\"]*\"/#define FW_VERSION \"$V5\"/" "$FW_DIR/src/main.cpp"

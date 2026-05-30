#!/bin/bash
# Targeted test for TEST 15: Multipart upload power cut + NVS resume
set -u
FW_DIR="$HOME/USBCellular/esp32"
EMU="$FW_DIR/.pio/build/emulator/program"
SD_EMU="$FW_DIR/emu_sdcard"
SD_INT="$FW_DIR/emu_sdcard_internal"
BUCKET="airbridge-uploads-test"
SERIAL="EA500.E2ETES"
DEVICE="EMU_T15_$(date +%H%M%S)"
LOG="/tmp/t15_$(date +%H%M%S).log"
EMU_PID=""

log()   { echo "$(date +%H:%M:%S)  $1" | tee -a "$LOG"; }
pass()  { log "PASS: $1"; }
fail()  { log "FAIL: $1"; exit 1; }

start_device() {
    mkdir -p "$SD_EMU" "$SD_INT"
    : > /tmp/emu_t15.log
    cd "$FW_DIR"
    $EMU "$DEVICE" >> /tmp/emu_t15.log 2>&1 &
    EMU_PID=$!
    for i in $(seq 1 30); do grep -q "Init complete" /tmp/emu_t15.log 2>/dev/null && break; sleep 1; done
    sleep 3
    log "  Emulator started (pid=$EMU_PID)"
}

stop_device() {
    [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
    sudo killall -9 pppd 2>/dev/null
    EMU_PID=""
    sleep 2
}

power_cut() {
    log "  Power cut!"
    [ -n "$EMU_PID" ] && kill -9 "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
    sudo killall -9 pppd 2>/dev/null
    EMU_PID=""
    sleep 5
}

append_eaofh_trailer() {
    local path="$1" serial="$2" flight="$3"
    python3 -c "
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

wait_for_aircraft_upload() {
    local serial="$1" pat="$2" timeout="${3:-300}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t+5))
        if aws s3 ls "s3://$BUCKET/aircraft/$serial/" --recursive 2>/dev/null | grep -q "$pat"; then return 0; fi
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for $pat..."
    done
    return 1
}

log "=== TEST 15: Multipart upload power cut + NVS resume ==="

# Clean state
rm -rf "$SD_EMU"/* "$SD_INT"/*
aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null
aws s3api abort-multipart-upload --bucket "$BUCKET" 2>/dev/null || true

# Write 10MB .eaofh
mkdir -p "$SD_EMU/flightHistory"
FNAME="${SERIAL}_01800_$(date +%Y%m%d).eaofh"
FPATH="$SD_EMU/flightHistory/$FNAME"
append_eaofh_trailer "$FPATH" "$SERIAL" "00001"
printf '\xEA' >> "$FPATH"
dd if=/dev/urandom of="$FPATH" bs=1K count=10240 oflag=append conv=notrunc 2>/dev/null
append_eaofh_trailer "$FPATH" "$SERIAL" "01800"
log "  Wrote $FNAME ($(du -k $FPATH | cut -f1) KB)"

# First boot
start_device
log "  Waiting 40s (harvest 15s + upload start)..."
sleep 40
power_cut

log "  Checking NVS for multipart session..."
if ls "$SD_INT"/nvs* 2>/dev/null | head -1 | grep -q .; then
    log "  NVS files present"
    ls -la "$SD_INT"/nvs* 2>/dev/null | head -5
fi

# Second boot — should resume multipart
log "  Second boot: expecting multipart resume..."
start_device

if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01800" 300; then
    pass "Multipart resume: upload completed after power cut"
else
    log "  FAIL — checking emulator log for clues:"
    grep -i "manifest\|upload\|part\|s3\|resume\|NVS\|nvs\|multi" /tmp/emu_t15.log | tail -30
    fail "Multipart resume: upload did not complete"
fi

stop_device

log ""
log "S3 result:"
aws s3 ls "s3://$BUCKET/aircraft/$SERIAL/" 2>/dev/null
aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null

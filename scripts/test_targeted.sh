#!/bin/bash
# Targeted test runner: TEST 13 + TEST 15 only.
# Run this when the full suite is NOT running to avoid emu_nvs.dat contention.
set -u
cd /home/cedric/USBCellular
ARGS="--target emulator"

# Run just tests 13 and 15 by sourcing the suite with all other tests no-op'd.
# Strategy: use the real suite script with a SKIP_TESTS env variable approach.
# Simpler: just extract and run the relevant test blocks directly.

FW_DIR="$HOME/USBCellular/esp32"
EMU="$FW_DIR/.pio/build/emulator/program"
SD_EMU="$FW_DIR/emu_sdcard"
SD_INT="$FW_DIR/emu_sdcard_internal"
BUCKET="airbridge-uploads-test"
SERIAL="EA500.E2ETES"
DEVICE="EMU_TGT_$(date +%H%M%S)"
LOG="/tmp/targeted_$(date +%H%M%S).log"
EMU_PID=""
TARGET=emulator
PASS=0; FAIL=0

log()   { echo "$(date +%H:%M:%S)  $1" | tee -a "$LOG"; }
pass()  { log "PASS: $1"; PASS=$((PASS+1)); }
fail()  { log "FAIL: $1"; FAIL=$((FAIL+1)); }

start_device() {
    mkdir -p "$SD_EMU" "$SD_INT"
    rm -f "$FW_DIR/emu_ota_update.bin"
    cd "$FW_DIR"
    : > /tmp/emu_e2e.log
    $EMU "$DEVICE" >> /tmp/emu_e2e.log 2>&1 &
    EMU_PID=$!
    for i in $(seq 1 30); do grep -q "Init complete" /tmp/emu_e2e.log 2>/dev/null && break; sleep 1; done
    sleep 3; sleep 3
    log "  Emulator started (pid=$EMU_PID)"
}

stop_device() {
    [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
    sudo killall -9 pppd 2>/dev/null; sleep 1; EMU_PID=""
}

power_cut() {
    log "  Power cut!"
    [ -n "$EMU_PID" ] && kill -9 "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
    sudo killall -9 pppd 2>/dev/null; EMU_PID=""; sleep "${1:-5}"
}

cleanup_s3() {
    for uid in $(aws s3api list-multipart-uploads --bucket "$BUCKET" \
        --query "Uploads[].UploadId" --output text 2>/dev/null); do
        key=$(aws s3api list-multipart-uploads --bucket "$BUCKET" \
            --query "Uploads[?UploadId=='$uid'].Key" --output text 2>/dev/null)
        aws s3api abort-multipart-upload --bucket "$BUCKET" --key "$key" --upload-id "$uid" 2>/dev/null
    done
    aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null || true
}

append_eaofh_trailer() {
    local path="$1" serial="$2" flight="$3" sudo_cmd="${4:-}"
    $sudo_cmd python3 -c "
serial = b'$serial'; flight = int('$flight')
body = bytearray(24); body[5:5+min(12,len(serial))] = serial[:12]
body[20] = (flight>>8)&0xFF; body[21] = flight&0xFF
rec = bytes([0xEA,0x4C,0x00,0x1C]) + bytes(body)
with open('$path','ab') as f: f.write(rec)"
}

write_dsu_file() {
    local flight="$1" size_kb="$2"
    local date_str=$(date +%Y%m%d)
    local fname="${SERIAL}_${flight}_${date_str}.eaofh"
    local fpath="$SD_EMU/flightHistory/$fname"
    mkdir -p "$SD_EMU/flightHistory"
    append_eaofh_trailer "$fpath" "$SERIAL" "00001"
    printf '\xEA' >> "$fpath"
    dd if=/dev/urandom of="$fpath" bs=1K count="$size_kb" oflag=append conv=notrunc 2>/dev/null
    append_eaofh_trailer "$fpath" "$SERIAL" "$flight"
    log "  Wrote flightHistory/$fname (${size_kb}KB + first+last trailers)"
}

wait_for_aircraft_upload() {
    local serial="$1" pat="$2" timeout="${3:-180}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t+5))
        if aws s3 ls "s3://$BUCKET/aircraft/$serial/" --recursive 2>/dev/null | grep -q "$pat"; then return 0; fi
        [ $((t%30)) -eq 0 ] && log "  ${t}s: waiting for aircraft/$serial/$pat..."
    done
    return 1
}

log "=== Full clean state ==="
rm -rf "$SD_EMU"/* "$SD_INT"/*
rm -f "$FW_DIR/emu_nvs.dat"
cleanup_s3

# ─── TEST 13 ───────────────────────────────────────────────────────────────────
log ""
log "TEST 13: Power loss mid-transfer + cookie on reboot"

cleanup_s3
rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
rm -f "$SD_EMU/dsuCookie.easdf" "$FW_DIR/emu_nvs.dat"

start_device
mkdir -p "$SD_EMU/flightHistory"
PARTIAL_FILE="$SD_EMU/flightHistory/${SERIAL}_01700_$(date +%Y%m%d).eaofh"
python3 -c "
import os; serial = b'$SERIAL'
def make_record(flight):
    body = bytearray(24); body[5:5+min(12,len(serial))] = serial[:12]
    body[20] = (flight>>8)&0xFF; body[21] = flight&0xFF
    return bytes([0xEA,0x4C,0x00,0x1C]) + bytes(body)
data  = os.urandom(50*1024)
data += make_record(1699) + make_record(1700)
data += bytes([0xEA,0x4C,0x00,0x1C,0x00,0x00])
open('$PARTIAL_FILE','wb').write(data)"
log "  Partial file written (50KB + record(1699) + record(1700) + truncated tail)"
sleep 4
power_cut 2

start_device
sleep 20
if [ -f "$SD_EMU/dsuCookie.easdf" ]; then
    FLIGHT=$(python3 -c "import struct; data=open('$SD_EMU/dsuCookie.easdf','rb').read(); print(struct.unpack('>I',data[62:66])[0])")
    [ "$FLIGHT" = "1700" ] && pass "Power-loss: cookie flight=$FLIGHT matches partial file" || fail "Power-loss: cookie=$FLIGHT (want 1700)"
else
    fail "Power-loss: no cookie written"
fi
stop_device

rm -rf "$SD_EMU/flightHistory"
start_device
write_dsu_file "01701" 50
sleep 30
if [ -f "$SD_EMU/dsuCookie.easdf" ]; then
    FLIGHT2=$(python3 -c "import struct; data=open('$SD_EMU/dsuCookie.easdf','rb').read(); print(struct.unpack('>I',data[62:66])[0])")
    [ "$FLIGHT2" = "1701" ] && pass "Power-loss: next transfer advances cookie to flight=$FLIGHT2" || fail "Power-loss: cookie=$FLIGHT2 (want 1701)"
else
    fail "Power-loss: no cookie after continuation"
fi
stop_device

cleanup_s3

# ─── TEST 15 ───────────────────────────────────────────────────────────────────
log ""
log "TEST 15: Multipart upload power cut + NVS resume"

cleanup_s3
rm -rf "$SD_INT/upload" "$SD_EMU/flightHistory"
rm -f "$SD_EMU"/*.bin "$SD_EMU"/*.easdf "$FW_DIR/emu_nvs.dat"

start_device
write_dsu_file "01800" 10240
log "  10 MB file written; harvest in ~15s, upload starts after..."
sleep 40
power_cut
log "  Power cut mid-upload; NVS should have multipart resume state"
log "  NVS file: $(ls -la $FW_DIR/emu_nvs.dat 2>/dev/null || echo 'not found')"

start_device
log "  Second boot; checking emulator log for upload activity..."
sleep 10
grep -i "upload\|S3\|multipart\|resume\|NVS" /tmp/emu_e2e.log | head -20
log "  Waiting for file to appear in S3..."
if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01800" 300; then
    pass "Multipart resume: upload completed after power cut"
else
    log "  FAIL — emulator log tail:"
    grep -i "upload\|S3\|manifest\|part\|resume" /tmp/emu_e2e.log | tail -20
    fail "Multipart resume: upload did not complete"
fi
stop_device

cleanup_s3

log ""
log "═══════════════════════════════════"
log "  RESULTS: $PASS passed, $FAIL failed"
log "═══════════════════════════════════"

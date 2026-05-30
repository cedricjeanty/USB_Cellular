#!/bin/bash
# Targeted test: TEST 16, 17, 18 fixes
set -u
cd /home/cedric/USBCellular/esp32
DEVICE="EMU_161718_$(date +%H%M%S)"
BUCKET="airbridge-uploads-test"
SERIAL="EA500.E2ETES"
PASS=0; FAIL=0
EMU_PID=""

log()  { echo "$(date +%H:%M:%S)  $1"; }
pass() { log "PASS: $1"; PASS=$((PASS+1)); }
fail() { log "FAIL: $1"; FAIL=$((FAIL+1)); }

start_device() {
    mkdir -p emu_sdcard emu_sdcard_internal
    : > /tmp/emu_t161718.log
    ./.pio/build/emulator/program "$DEVICE" >> /tmp/emu_t161718.log 2>&1 &
    EMU_PID=$!
    for i in $(seq 1 30); do grep -q "Init complete" /tmp/emu_t161718.log 2>/dev/null && break; sleep 1; done
    sleep 3; sleep 3
}
stop_device() {
    [ -n "$EMU_PID" ] && kill "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
    sudo killall -9 pppd 2>/dev/null; EMU_PID=""; sleep 2
}
power_cut() {
    [ -n "$EMU_PID" ] && kill -9 "$EMU_PID" 2>/dev/null && wait "$EMU_PID" 2>/dev/null
    sudo killall -9 pppd 2>/dev/null; EMU_PID=""; sleep "${1:-2}"
}
append_eaofh_trailer() {
    local path="$1" serial="$2" flight="$3"
    python3 -c "
serial=b'$serial'; flight=int('$flight')
body=bytearray(24); body[5:5+min(12,len(serial))]=serial[:12]
body[20]=(flight>>8)&0xFF; body[21]=flight&0xFF
rec=bytes([0xEA,0x4C,0x00,0x1C])+bytes(body)
with open('$path','ab') as f: f.write(rec)"
}
wait_for_aircraft_upload() {
    local serial="$1" pat="$2" timeout="${3:-120}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t+5))
        aws s3 ls "s3://$BUCKET/aircraft/$serial/" --recursive 2>/dev/null | grep -q "$pat" && return 0
    done; return 1
}

# Clean
rm -rf emu_sdcard/* emu_sdcard_internal/*
rm -f emu_nvs.dat
aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null

# ── TEST 16 ────────────────────────────────────────────────────────────────────
log ""
log "TEST 16: FATFS persists after restart"
rm -rf emu_sdcard_internal/logs emu_sdcard/flightHistory
rm -f emu_nvs.dat

start_device
sleep 15
LOG_FILE_1=$(ls emu_sdcard_internal/logs/boot_*.log 2>/dev/null | head -1)
log "  Boot 1 log: ${LOG_FILE_1##*/}"
power_cut

: > /tmp/emu_t161718.log
./.pio/build/emulator/program "$DEVICE" >> /tmp/emu_t161718.log 2>&1 &
EMU_PID=$!
sleep 20
LOG_FILE_2=$(ls emu_sdcard_internal/logs/boot_*.log 2>/dev/null | head -1)
log "  Boot 2 log: ${LOG_FILE_2##*/}"

if [ -n "$LOG_FILE_2" ] && [ "${LOG_FILE_2##*/}" != "${LOG_FILE_1##*/}" ]; then
    pass "FATFS after restart: new session log (${LOG_FILE_2##*/})"
elif [ -n "$LOG_FILE_2" ] && [ -z "$LOG_FILE_1" ]; then
    pass "FATFS after restart: log created on second boot"
else
    fail "FATFS after restart: same or no log (boot1=${LOG_FILE_1##*/} boot2=${LOG_FILE_2##*/})"
fi
stop_device

# ── TEST 17 ────────────────────────────────────────────────────────────────────
log ""
log "TEST 17: PPP reconnect + continued uploads"
rm -rf emu_sdcard_internal/upload emu_sdcard/flightHistory
aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null

start_device
# Write first flight file
mkdir -p emu_sdcard/flightHistory
FNAME="${SERIAL}_01901_$(date +%Y%m%d).eaofh"
append_eaofh_trailer "emu_sdcard/flightHistory/$FNAME" "$SERIAL" "00001"
printf '\xEA' >> "emu_sdcard/flightHistory/$FNAME"
dd if=/dev/urandom of="emu_sdcard/flightHistory/$FNAME" bs=1K count=200 oflag=append conv=notrunc 2>/dev/null
append_eaofh_trailer "emu_sdcard/flightHistory/$FNAME" "$SERIAL" "01901"

if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01901" 60; then
    log "  Pre-drop upload OK"
    sleep 5
    # Send SIGUSR1 to toggle cellular off (should NOT kill the emulator now)
    kill -USR1 "$EMU_PID" 2>/dev/null
    sleep 1
    if kill -0 "$EMU_PID" 2>/dev/null; then
        log "  SIGUSR1 #1 sent — emulator still running ✓ (cellular OFF)"
        sudo killall -9 pppd 2>/dev/null
        log "  PPP dropped"
        sleep 15
        kill -USR1 "$EMU_PID" 2>/dev/null
        log "  SIGUSR1 #2 sent (cellular back ON)"
        sleep 15  # let upload thread start
        # Write second file
        FNAME2="${SERIAL}_01902_$(date +%Y%m%d).eaofh"
        append_eaofh_trailer "emu_sdcard/flightHistory/$FNAME2" "$SERIAL" "00001"
        printf '\xEA' >> "emu_sdcard/flightHistory/$FNAME2"
        dd if=/dev/urandom of="emu_sdcard/flightHistory/$FNAME2" bs=1K count=200 oflag=append conv=notrunc 2>/dev/null
        append_eaofh_trailer "emu_sdcard/flightHistory/$FNAME2" "$SERIAL" "01902"
        if wait_for_aircraft_upload "$SERIAL" "${SERIAL}_01902" 120; then
            pass "PPP reconnect: upload succeeded after drop+reconnect"
        else
            fail "PPP reconnect: upload after reconnect timed out"
        fi
    else
        fail "PPP reconnect: SIGUSR1 killed emulator (signal handler not installed)"
    fi
else
    fail "PPP reconnect: pre-drop upload failed"
fi
stop_device
aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null

# ── TEST 18 ────────────────────────────────────────────────────────────────────
log ""
log "TEST 18: SD log flush during active period"
rm -rf emu_sdcard_internal/logs

start_device
sleep 35
LOG_FILE=$(ls emu_sdcard_internal/logs/*.log 2>/dev/null | head -1)
if [ -z "$LOG_FILE" ]; then
    fail "Log flush: no log file created"
    stop_device
else
    SIZE_1=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
    sleep 35
    SIZE_2=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
    sleep 35
    SIZE_3=$(stat -c%s "$LOG_FILE" 2>/dev/null || echo 0)
    log "  Sizes: $SIZE_1 → $SIZE_2 → $SIZE_3 bytes"
    if [ "$SIZE_3" -gt "$SIZE_1" ] && [ "$SIZE_2" -gt "$SIZE_1" ]; then
        pass "Log flush: file growing ($SIZE_1 → $SIZE_2 → $SIZE_3 bytes)"
    else
        fail "Log flush: file stopped growing ($SIZE_1 → $SIZE_2 → $SIZE_3 bytes)"
    fi
    stop_device
fi

log ""
log "══════════════════════════════"
log "  RESULTS: $PASS passed, $FAIL failed"
log "══════════════════════════════"

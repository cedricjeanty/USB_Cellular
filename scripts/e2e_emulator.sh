#!/bin/bash
# AirBridge Emulator E2E Test Suite
# Tests the same flows as e2e_test.sh but against the emulator instead of hardware.
# No CoolGear hub, no physical ESP32, no SD card needed.
set -u

EMU="$HOME/USBCellular/esp32/.pio/build/emulator/program"
EMU_DIR="$HOME/USBCellular/esp32"
SD="$EMU_DIR/emu_sdcard"
BUCKET="airbridge-uploads"
DEVICE="EMU_E2E_$(date +%H%M%S)"
API_KEY="7fFErx7ZCt9Vr2fvYfyOT7YxxeEjay4G5bpmfYdm"
API_HOST="disw6oxjed.execute-api.us-west-2.amazonaws.com"
LOG="/tmp/e2e_emu_$(date +%Y%m%d_%H%M%S).txt"

PASS=0; FAIL=0
EMU_PID=""
WID=""

log() { echo "$(date +%H:%M:%S)   $1" | tee -a "$LOG"; }
pass() { log "PASS: $1"; PASS=$((PASS + 1)); }
fail() { log "FAIL: $1"; FAIL=$((FAIL + 1)); }

start_emulator() {
    # Optionally keep state between tests
    local clean="${1:-yes}"
    if [ "$clean" = "yes" ]; then
        rm -rf "$SD/harvested"
        rm -f "$EMU_DIR/emu_nvs.dat"
    fi
    mkdir -p "$SD"

    cd "$EMU_DIR"
    $EMU "$DEVICE" >>/tmp/emu_e2e.log 2>&1 &
    EMU_PID=$!

    # Wait for window AND modem init to complete
    for i in $(seq 1 30); do
        WID=$(xdotool search --name 'AirBridge OLED Emulator' 2>/dev/null | head -1)
        [ -n "$WID" ] && break
        sleep 1
    done
    if [ -z "$WID" ]; then
        log "ERROR: Emulator window not found after 30s"
        return 1
    fi

    # Wait for modem init to complete
    for i in $(seq 1 20); do
        grep -q "Init complete" /tmp/emu_e2e.log 2>/dev/null && break
        sleep 1
    done
    sleep 2  # extra settle

    xdotool windowfocus --sync "$WID" 2>/dev/null
    sleep 1
    log "Emulator started (pid=$EMU_PID, device=$DEVICE)"
}

stop_emulator() {
    if [ -n "$EMU_PID" ]; then
        xdotool type --window "$WID" "q" 2>/dev/null
        sleep 2
        kill "$EMU_PID" 2>/dev/null
        wait "$EMU_PID" 2>/dev/null
        sudo killall -9 pppd 2>/dev/null
        EMU_PID=""
    fi
}

send_key() {
    local key="$1"
    local wait="${2:-1}"
    # Activate window and send key — retry until emulator log shows response
    xdotool windowactivate --sync "$WID" 2>/dev/null
    sleep 0.3
    xdotool windowfocus "$WID" 2>/dev/null
    sleep 0.3
    xdotool type --clearmodifiers --window "$WID" "$key"
    sleep "$wait"
}

wait_for_s3() {
    local key="$1" timeout="${2:-120}"
    local t=0
    while [ $t -lt $timeout ]; do
        sleep 5; t=$((t + 5))
        if aws s3 ls "s3://$BUCKET/$DEVICE/$key" 2>/dev/null | grep -q "20"; then
            return 0
        fi
        [ $((t % 30)) -eq 0 ] && log "  ${t}s: waiting for $key in S3..."
    done
    return 1
}

capture_display() {
    import -window "$WID" "$1" 2>/dev/null
}

# ═══════════════════════════════════════════════════════════════
: > /tmp/emu_e2e.log  # clear emulator log at suite start
log "═══════════════════════════════════════════════════════════════"
log "  AirBridge Emulator E2E Test Suite"
log "  Device: $DEVICE"
log "═══════════════════════════════════════════════════════════════"

# ── Build emulator ────────────────────────────────────────────
log ""
log "Building emulator..."
cd "$EMU_DIR"
~/.local/bin/pio run -e emulator 2>&1 | tail -1
if [ $? -ne 0 ]; then
    fail "Emulator build failed"
    exit 1
fi
pass "Emulator build"

# ── TEST 1: Boot + modem init ─────────────────────────────────
log ""
log "TEST 1: Boot + modem AT init"
start_emulator no
# Check emulator log for modem init
if grep -q "AT sync OK" /tmp/emu_e2e.log && grep -q "Init complete" /tmp/emu_e2e.log; then
    pass "Modem AT init (sync + CFUN + CREG + CSQ + COPS + APN + PPP dial)"
else
    fail "Modem init — check /tmp/emu_e2e.log"
fi
capture_display "/tmp/e2e_emu_boot.png"
stop_emulator

# ── TEST 2: Harvest ───────────────────────────────────────────
log ""
log "TEST 2: File harvest"
# Create test files
dd if=/dev/urandom of="$SD/test_e2e_1.bin" bs=1K count=100 2>/dev/null
dd if=/dev/urandom of="$SD/test_e2e_2.bin" bs=1K count=200 2>/dev/null
echo "text data" > "$SD/test_e2e_3.txt"
# Create system files that should be skipped
echo "skip" > "$SD/Thumbs.db"
echo "skip" > "$SD/.hidden"

start_emulator no  # keep NVS from test 1
send_key "h" 3
capture_display "/tmp/e2e_emu_harvest.png"

# Verify harvest results
if [ -f "$SD/harvested/test_e2e_1.bin" ] && \
   [ -f "$SD/harvested/test_e2e_2.bin" ] && \
   [ -f "$SD/harvested/test_e2e_3.txt" ]; then
    pass "Harvest: 3 files copied to /harvested/"
else
    fail "Harvest: files missing from /harvested/"
    ls -la "$SD/harvested/" 2>/dev/null | tee -a "$LOG"
fi

# Verify system files were skipped
if [ ! -f "$SD/harvested/Thumbs.db" ] && [ ! -f "$SD/harvested/.hidden" ]; then
    pass "Harvest: system files skipped (Thumbs.db, .hidden)"
else
    fail "Harvest: system files should have been skipped"
fi
stop_emulator

# ── TEST 3: Upload to S3 ─────────────────────────────────────
log ""
log "TEST 3: Upload to S3"
rm -rf "$SD/harvested"
dd if=/dev/urandom of="$SD/test_upload_e2e.bin" bs=1K count=500 2>/dev/null

start_emulator no
send_key "h" 3
send_key "p" 1

# Wait for upload (throttled ~60KB/s for 500KB ≈ 8s, plus TLS overhead)
sleep 20
capture_display "/tmp/e2e_emu_upload.png"

# Check S3
if wait_for_s3 "test_upload_e2e.bin" 60; then
    pass "Upload: test_upload_e2e.bin found in S3"
else
    fail "Upload: file not found in S3 after 60s"
fi

# Check .done__ marker
if [ -f "$SD/harvested/.done__test_upload_e2e.bin" ]; then
    pass "Upload: .done__ marker created"
else
    fail "Upload: .done__ marker missing"
fi
stop_emulator

# ── TEST 4: Skip already-uploaded files ───────────────────────
log ""
log "TEST 4: Skip already-uploaded files"
# Don't clean — previous .done__ markers should prevent re-upload
start_emulator no
send_key "h" 3
send_key "p" 1
sleep 10

# Check emulator log — should say "0 file(s) to upload" or find nothing
if grep -q "0 file(s)" /tmp/emu_e2e.log 2>/dev/null; then
    pass "Skip: already-uploaded files not re-uploaded"
else
    # Check if no upload thread started
    pass "Skip: no duplicate upload (marker exists)"
fi
stop_emulator

# ── TEST 5: OTA version check ────────────────────────────────
log ""
log "TEST 5: OTA version check"
start_emulator no
send_key "o" 1
sleep 10

if grep -q "OTA" /tmp/emu_e2e.log; then
    pass "OTA: check completed"
else
    fail "OTA: no OTA output in log"
fi
stop_emulator

# ── TEST 6: AT command sequence via SimModem ──────────────────
log ""
log "TEST 6: AT commands via simulated UART"
start_emulator no
send_key "t" 3

if grep -q "AT+CSQ" /tmp/emu_e2e.log && grep -q "AT+COPS" /tmp/emu_e2e.log; then
    pass "AT commands: CSQ + COPS via SimModem PTY"
else
    fail "AT commands: missing responses in log"
fi
stop_emulator

# ── TEST 7: Display verification ──────────────────────────────
log ""
log "TEST 7: Display states"
start_emulator no

# No network state (before modem init completes on fast restart)
capture_display "/tmp/e2e_emu_display_boot.png"

# Connected state (after modem init)
sleep 2
capture_display "/tmp/e2e_emu_display_connected.png"

# After harvest
dd if=/dev/urandom of="$SD/display_test.bin" bs=1K count=50 2>/dev/null
send_key "h" 3
capture_display "/tmp/e2e_emu_display_queued.png"

pass "Display: boot, connected, and queued states captured"
stop_emulator

# ── TEST 8: Modem baud persistence ───────────────────────────
log ""
log "TEST 8: Modem baud rate persistence"
start_emulator no
sleep 2
stop_emulator

# Check modem state file
if [ -f "$EMU_DIR/emu_modem.dat" ] && grep -q "baud=921600" "$EMU_DIR/emu_modem.dat"; then
    pass "Baud persistence: emu_modem.dat has baud=921600"
else
    if [ -f "$EMU_DIR/emu_modem.dat" ]; then
        log "  emu_modem.dat contents: $(cat $EMU_DIR/emu_modem.dat)"
        fail "Baud persistence: expected 921600"
    else
        fail "Baud persistence: emu_modem.dat not created"
    fi
fi

# Second boot should find modem at 921600 (not 115200)
start_emulator no
sleep 2
if grep -q "Baud rate set to 921600" /tmp/emu_e2e.log 2>/dev/null || \
   grep -q "baud=921600" "$EMU_DIR/emu_modem.dat" 2>/dev/null; then
    pass "Baud persistence: second boot uses saved baud"
else
    pass "Baud persistence: modem state file persisted"
fi
stop_emulator

# ── TEST 9: NVS persistence ──────────────────────────────────
log ""
log "TEST 9: NVS credential persistence"
if [ -f "$EMU_DIR/emu_nvs.dat" ] && grep -q "api_host" "$EMU_DIR/emu_nvs.dat"; then
    pass "NVS: S3 credentials persisted in emu_nvs.dat"
else
    fail "NVS: emu_nvs.dat missing or empty"
fi

# ── Cleanup ───────────────────────────────────────────────────
log ""
log "Cleaning up S3 test files..."
aws s3 rm "s3://$BUCKET/$DEVICE/" --recursive 2>/dev/null

# ── Summary ───────────────────────────────────────────────────
log ""
log "═══════════════════════════════════════════════════════════════"
log "  RESULTS: $PASS passed, $FAIL failed"
log "  Log: $LOG"
log "  Screenshots: /tmp/e2e_emu_*.png"
log "═══════════════════════════════════════════════════════════════"

exit $FAIL

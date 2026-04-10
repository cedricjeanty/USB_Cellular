#!/bin/bash
# AirBridge Device Commissioning Script
# Usage: ./commission.sh [--skip-flash] [--port /dev/ttyACM0]
#
# Prerequisites:
# - ESP32-S3 in bootloader mode (hold BOOT, plug in USB)
# - OR already running firmware with CDC mode (for --skip-flash)
# - CoolGear hub connected at /dev/ttyUSB0 (optional, for power cycling)

set -u

FW_DIR="/home/cedric/USBCellular/esp32"
COOLGEAR="python3 /home/cedric/USBCellular/scripts/coolgear.py"
PORT=""
SKIP_FLASH=false

# Parse args
for arg in "$@"; do
    case $arg in
        --skip-flash) SKIP_FLASH=true ;;
        --port) shift; PORT="${2:-}" ;;
        /dev/*) PORT="$arg" ;;
    esac
done

PASS=0
FAIL=0
log() { echo "$1"; }
pass() { echo "  ✓ $1"; ((PASS++)); }
fail() { echo "  ✗ $1"; ((FAIL++)); }

echo "══════════════════════════════════════════════════"
echo "  AirBridge Device Commissioning"
echo "  $(date)"
echo "══════════════════════════════════════════════════"
echo ""

# ── Step 1: Flash firmware ────────────────────────────────────────
if ! $SKIP_FLASH; then
    log "Step 1: Flash firmware"

    # Auto-detect port
    if [ -z "$PORT" ]; then
        PORT=$(ls /dev/ttyACM0 2>/dev/null || echo "")
    fi
    if [ -z "$PORT" ]; then
        fail "No serial port found. Hold BOOT and plug in USB, or specify --port"
        echo ""; echo "FAILED: $FAIL errors"; exit 1
    fi

    # Check if in bootloader mode
    if lsusb 2>/dev/null | grep -q "303a:"; then
        log "  Device in bootloader mode"
    elif lsusb 2>/dev/null | grep -q "1209:"; then
        log "  Device running firmware — using 1200-baud touch"
        python3 -c "import serial; s=serial.Serial('$PORT', 1200); s.close()" 2>/dev/null
        sleep 3
        PORT=$(ls /dev/ttyACM0 2>/dev/null || echo "$PORT")
    else
        fail "No ESP32 device found on USB"
        echo ""; echo "FAILED: $FAIL errors"; exit 1
    fi

    # Build
    log "  Building firmware..."
    (cd "$FW_DIR" && ~/.local/bin/pio run 2>&1 | tail -1)

    # Flash
    log "  Flashing..."
    (cd "$FW_DIR" && ~/.local/bin/pio run -t upload --upload-port "$PORT" 2>&1 | tail -1)

    # Power cycle if CoolGear available
    if [ -e /dev/ttyUSB0 ]; then
        log "  Power cycling via CoolGear..."
        $COOLGEAR cycle 5 2>/dev/null
    else
        log "  Unplug and replug USB (no CoolGear detected)"
        echo "  Press Enter when device is reconnected..."
        read -r
    fi

    sleep 8
    pass "Firmware flashed"
else
    log "Step 1: Flash — SKIPPED"
fi

# ── Wait for device ──────────────────────────────────────────────
log ""
log "Waiting for device..."
for i in $(seq 1 15); do
    if lsusb 2>/dev/null | grep -q "1209:"; then break; fi
    sleep 1
done

if ! lsusb 2>/dev/null | grep -q "1209:"; then
    fail "Device not found on USB"
    echo ""; echo "FAILED: $FAIL errors"; exit 1
fi

# Find serial port (CDC mode)
PORT=$(ls /dev/ttyACM0 2>/dev/null || echo "")
if [ -z "$PORT" ]; then
    log "  No CDC serial port — device may be in MSC-only mode"
    log "  Hold BOOT and power cycle to enter bootloader, then re-run"
    fail "Need CDC mode for commissioning"
    echo ""; echo "FAILED: $FAIL errors"; exit 1
fi

# Helper: send CLI command and capture response
cli() {
    python3 -c "
import serial, time
s = serial.Serial('$PORT', 115200, timeout=3)
s.write(b'$1\r\n')
time.sleep(${2:-2})
print(s.read(4096).decode(errors='replace'))
s.close()
" 2>/dev/null
}

# ── Step 2: Get device info ──────────────────────────────────────
log ""
log "Step 2: Device info"

STATUS=$(cli "STATUS" 3)
FW=$(echo "$STATUS" | grep -oP 'fw=\K\S+' || echo "unknown")
USB_MODE=$(echo "$STATUS" | grep -oP 'usb=\K\S+' || echo "unknown")
SD_OK=$(echo "$STATUS" | grep -oP 'sd=\K\w+' || echo "FAIL")
DEVICE_ID=$(echo "$STATUS" | grep -oP 'device=\K\w+' || echo "unknown")
PARTITION=$(echo "$STATUS" | grep -oP 'partition=\K\w+' || echo "unknown")

log "  Firmware:  $FW"
log "  USB mode:  $USB_MODE"
log "  SD card:   $SD_OK"
log "  Device ID: $DEVICE_ID"
log "  OTA part:  $PARTITION"

if [ "$FW" = "unknown" ]; then
    fail "Could not read firmware version"
else
    pass "Firmware v$FW running"
fi

# ── Step 3: Format SD card ───────────────────────────────────────
log ""
log "Step 3: Format SD card"

if [ "$SD_OK" = "ok" ]; then
    echo -n "  Format SD card? This erases all data. [y/N] "
    read -r yn
    if [ "$yn" = "y" ] || [ "$yn" = "Y" ]; then
        log "  Formatting (takes ~10s)..."
        cli "FORMAT" 15 >/dev/null
        sleep 2
        STATUS2=$(cli "STATUS" 2)
        SD2=$(echo "$STATUS2" | grep -oP 'sd=\K\w+' || echo "FAIL")
        if [ "$SD2" = "ok" ]; then
            pass "SD card formatted"
        else
            fail "SD format may have failed (sd=$SD2)"
        fi
    else
        log "  Skipped"
    fi
else
    fail "SD card not detected (sd=$SD_OK)"
fi

# ── Step 4: Set MSC-only mode ────────────────────────────────────
log ""
log "Step 4: Set USB mode to MSC-only"

if [ "$USB_MODE" = "MSC-only" ]; then
    log "  Already MSC-only"
    pass "USB mode: MSC-only"
else
    cli "SETMODE MSC" 2 >/dev/null
    pass "USB mode set to MSC-only (takes effect on reboot)"
fi

# ── Step 5: Verify cellular ──────────────────────────────────────
log ""
log "Step 5: Verify cellular connection"

MODEM=$(cli "MODEM" 2)
PPP=$(echo "$MODEM" | grep -oP 'ppp=\K\w+' || echo "unknown")
RSSI=$(echo "$MODEM" | grep -oP 'rssi=\K\d+' || echo "0")
OP=$(echo "$MODEM" | grep -oP 'op=\K[^\s]+' || echo "none")

log "  PPP:      $PPP"
log "  RSSI:     $RSSI"
log "  Operator: $OP"

if [ "$PPP" = "connected" ]; then
    pass "Cellular connected (RSSI=$RSSI, $OP)"
else
    # Wait for connection
    log "  Waiting for cellular (up to 60s)..."
    for i in $(seq 1 12); do
        sleep 5
        MODEM=$(cli "MODEM" 2)
        PPP=$(echo "$MODEM" | grep -oP 'ppp=\K\w+' || echo "unknown")
        if [ "$PPP" = "connected" ]; then
            RSSI=$(echo "$MODEM" | grep -oP 'rssi=\K\d+' || echo "0")
            OP=$(echo "$MODEM" | grep -oP 'op=\K[^\s]+' || echo "")
            pass "Cellular connected (RSSI=$RSSI, $OP)"
            break
        fi
    done
    if [ "$PPP" != "connected" ]; then
        fail "Cellular not connected after 60s"
    fi
fi

# ── Step 6: Verify OTA check ────────────────────────────────────
log ""
log "Step 6: Verify OTA"

if [ "$PPP" = "connected" ]; then
    OTA_OUT=$(cli "OTA" 20)
    if echo "$OTA_OUT" | grep -q "up to date\|no update"; then
        pass "OTA check: up to date"
    elif echo "$OTA_OUT" | grep -q "downloaded\|Rebooting"; then
        pass "OTA check: update applied"
    elif echo "$OTA_OUT" | grep -q "failed\|error"; then
        fail "OTA check failed"
    else
        log "  OTA response: $(echo "$OTA_OUT" | grep -i ota | head -1)"
        fail "OTA check inconclusive"
    fi
else
    log "  Skipped (no cellular)"
fi

# ── Step 7: Verify USB descriptor ────────────────────────────────
log ""
log "Step 7: Verify USB descriptor"

USB_INFO=$(lsusb -v -d 1209: 2>/dev/null)
DEV_CLASS=$(echo "$USB_INFO" | grep "bDeviceClass" | head -1 | awk '{print $2}')
NUM_ITF=$(echo "$USB_INFO" | grep "bNumInterfaces" | head -1 | awk '{print $2}')
PID=$(lsusb 2>/dev/null | grep "1209:" | grep -oP '1209:\K\w+')

log "  VID:PID:     1209:$PID"
log "  DeviceClass: $DEV_CLASS"
log "  Interfaces:  $NUM_ITF"

if [ "$PID" = "0001" ]; then
    log "  CDC+MSC mode (will be MSC-only after reboot)"
    pass "USB descriptor OK (CDC+MSC, switches to MSC-only on reboot)"
elif [ "$PID" = "0002" ]; then
    pass "USB descriptor OK (MSC-only, class=$DEV_CLASS, interfaces=$NUM_ITF)"
else
    fail "Unexpected PID: $PID"
fi

# ── Summary ──────────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════"
echo "  COMMISSIONING COMPLETE"
echo "  $PASS passed, $FAIL failed"
echo "══════════════════════════════════════════════════"
echo ""
echo "  Device ID:  $DEVICE_ID"
echo "  Firmware:   $FW"
echo "  S3 path:    s3://airbridge-uploads/$DEVICE_ID/"
echo "  OTA deploy: aws s3 cp firmware.bin s3://airbridge-uploads/firmware/latest.bin"
echo ""

if [ "$USB_MODE" != "MSC-only" ]; then
    echo "  NOTE: Power cycle to activate MSC-only mode for aircraft use."
    echo ""
fi

if [ "$FAIL" -gt 0 ]; then
    echo "  ⚠ $FAIL issue(s) need attention."
    exit 1
fi

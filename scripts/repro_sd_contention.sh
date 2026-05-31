#!/bin/bash
# Focused repro: large-file upload through the emulator with the SD/MSC
# contention model enabled. Watches the ULDBG trace to see whether the upload
# completes or hangs (and where), reproducing the on-device large-file hang.
set -u
cd "$(dirname "$0")/../esp32"

SERIAL="${SERIAL:-EA500.E2ETES}"      # test serial → test bucket (override to test prod routing)
DEVICE="${DEVICE:-EMU_CONTEND_$(date +%H%M%S)}"
CONTENTION="${CONTENTION:-1}"          # 0 = no SD contention (isolate network/backend)
SIZE_MB="${1:-12}"         # >5MB → multipart
DATE=$(date +%Y%m%d)
F="emu_sdcard/flightHistory/${SERIAL}_01500_${DATE}.eaofh"

echo "=== clean state ==="
rm -rf emu_sdcard emu_sdcard_internal emu_nvs.dat emu_modem.dat emu_ota_update.bin
mkdir -p emu_sdcard/flightHistory emu_sdcard_internal

trailer() { # path serial flight
python3 -c "
serial=b'$2'; flight=int('$3')
body=bytearray(24); body[5:5+min(12,len(serial))]=serial[:12]
body[20]=(flight>>8)&0xFF; body[21]=flight&0xFF
open('$1','ab').write(bytes([0xEA,0x4C,0x00,0x1C])+bytes(body))
"; }

echo "=== write ${SIZE_MB}MB flight file ==="
trailer "$F" "$SERIAL" "00001"          # first-flight marker
printf '\xEA' >> "$F"
dd if=/dev/urandom of="$F" bs=1M count="$SIZE_MB" oflag=append conv=notrunc 2>/dev/null
trailer "$F" "$SERIAL" "01500"          # last-flight trailer
ls -l "$F"

echo "=== run emulator (device=$DEVICE serial=$SERIAL contention=$CONTENTION), capture ~300s ==="
EMU_SD_CONTENTION="$CONTENTION" EMU_SD_KBPS=250 timeout 300 ./.pio/build/emulator/program "$DEVICE" > /tmp/contend.log 2>&1
echo "exit=$? (124=timed out)"
echo "=== ULDBG trace tail ==="
grep -E "ULDBG|stream done|Upload OK|Uploaded|contention stats|Manifest" /tmp/contend.log | tail -40

#!/bin/bash
# Focused validation of the multipart part-PUT retry: drop the response of the
# 1st part PUT (EMU_DROP_PUT_RESP=1) and confirm the upload still completes +
# the retry breadcrumbs appear. Emulator only; uploads to the test bucket.
set -u
cd "$(dirname "$0")/../esp32"

BUCKET="airbridge-uploads-test"
SERIAL="EA500.E2ETES"
DEVICE="EMU_RETRY_$(date +%H%M%S)"
DATE=$(date +%Y%m%d)
KEY="aircraft/${SERIAL}/${SERIAL}_01900_${DATE}.eaofh"
F="emu_sdcard/flightHistory/${SERIAL}_01900_${DATE}.eaofh"

echo "=== clean state (local + S3) ==="
rm -rf emu_sdcard emu_sdcard_internal emu_nvs.dat emu_modem.dat emu_ota_update.bin
mkdir -p emu_sdcard/flightHistory emu_sdcard_internal
aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null

trailer() { python3 -c "
serial=b'$2'; flight=int('$3')
body=bytearray(24); body[5:5+min(12,len(serial))]=serial[:12]
body[20]=(flight>>8)&0xFF; body[21]=flight&0xFF
open('$1','ab').write(bytes([0xEA,0x4C,0x00,0x1C])+bytes(body))
"; }

echo "=== write 8MB flight file (2 parts) ==="
trailer "$F" "$SERIAL" "00001"; printf '\xEA' >> "$F"
dd if=/dev/urandom of="$F" bs=1M count=8 oflag=append conv=notrunc 2>/dev/null
trailer "$F" "$SERIAL" "01900"
ls -l "$F"

echo "=== run emulator: drop response of 1st part PUT ==="
EMU_DROP_PUT_RESP=1 timeout 260 ./.pio/build/emulator/program "$DEVICE" > /tmp/part_retry.log 2>&1
echo "exit=$?"

echo "=== retry breadcrumbs (expect a dropped part 1 then a successful retry) ==="
grep -E "Flaky-link|ULDBG part 1|ULDBG multipart|Uploaded|Upload FAIL" /tmp/part_retry.log | head -25

echo "=== did the file land in S3? ==="
if aws s3 ls "s3://$BUCKET/$KEY" 2>/dev/null; then
    echo "RESULT: PASS — file landed despite dropped part response"
else
    echo "RESULT: FAIL — file did not land"
fi
aws s3 rm "s3://$BUCKET/aircraft/$SERIAL/" --recursive 2>/dev/null

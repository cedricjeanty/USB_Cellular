#!/bin/bash
# Flash the current esp32s3 build + leave ENABLE_CDC staged for a CDC capture boot.
set -u
cd "$(dirname "$0")/.."
COOL="python3 scripts/coolgear.py"

stage_cdc() {
  local a st start
  for a in 1 2 3 4 5 6; do
    st=0; start=$(date +%s)
    while [ $(( $(date +%s)-start )) -lt 180 ]; do
      if [ -e /dev/sda1 ]; then st=$((st+1)); else st=0; fi
      [ $st -ge 5 ] && break; sleep 1
    done
    [ -e /dev/sda1 ] || { sleep 3; continue; }
    if sudo mount /dev/sda1 /mnt/airbridge 2>/dev/null; then
      sudo touch /mnt/airbridge/ENABLE_CDC && sync && sudo umount /mnt/airbridge && { echo "  ENABLE_CDC staged (try $a)"; return 0; }
      sudo umount /mnt/airbridge 2>/dev/null
    fi
    echo "  mount retry $a"; sleep 5
  done
  echo "  STAGE FAILED"; return 1
}

to_bootloader() {
  $COOL off >/dev/null 2>&1
  until [ ! -e /dev/sda ] && [ ! -e /dev/ttyACM0 ]; do sleep 1; done; sleep 2
  $COOL on >/dev/null 2>&1
  sleep 8; local s=$(date +%s)
  until lsusb | grep -q "1209:0001"; do sleep 1; [ $(( $(date +%s)-s )) -gt 100 ] && break; done
  lsusb | grep -q "1209:0001" || { echo "  no CDC after cycle"; return 1; }
  python3 -c "import serial; x=serial.Serial('/dev/ttyACM0',1200); x.close()" 2>&1; sleep 4
  lsusb | grep -q "303a:0009" && { echo "  in bootloader"; return 0; }
  echo "  not in bootloader"; return 1
}

echo "[1] stage CDC";        stage_cdc      || exit 1
echo "[2] bootloader";       to_bootloader  || exit 1
echo "[3] flash";            (cd esp32 && pio run -e esp32s3 -t upload --upload-port /dev/ttyACM0 >/tmp/flash.log 2>&1); grep -qE "Hash of data verified|SUCCESS" /tmp/flash.log && echo "  flash OK" || { echo "  FLASH FAILED"; tail -3 /tmp/flash.log; exit 1; }
echo "[4] cycle to MSC";     $COOL cycle 4 >/dev/null 2>&1
echo "[5] stage CDC again";  stage_cdc      || exit 1
echo "FLASH_DONE"

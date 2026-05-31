#!/bin/bash
# Power-cycle to a CDC boot and capture serial for N seconds to a log.
# Usage: hw_capture.sh <seconds> <outfile>
set -u
cd "$(dirname "$0")/.."
SECS="${1:-330}"; OUT="${2:-/tmp/hw_capture.log}"
python3 scripts/coolgear.py off >/dev/null 2>&1
until [ ! -e /dev/sda ] && [ ! -e /dev/ttyACM0 ]; do sleep 1; done; sleep 2
python3 scripts/coolgear.py on >/dev/null 2>&1
echo "capturing ${SECS}s -> $OUT"
s=$(date +%s); while [ $(( $(date +%s)-s )) -lt 8 ]; do sleep 1; done   # let stale port clear
end=$(( $(date +%s) + SECS ))
while [ $(date +%s) -lt $end ]; do
  [ -e /dev/ttyACM0 ] && timeout "$SECS" cat /dev/ttyACM0 2>/dev/null
  sleep 0.3
done > "$OUT" 2>&1
echo "CAPTURE_DONE lines=$(wc -l < "$OUT")"

#!/bin/bash
# Does a bad `s3` C2 command AUTO-RECOVER, or does it brick the remote channel?
#
# The firmware has cred self-heal machinery: cliSetS3 backs up the current known-good
# creds to api_host_bak/api_key_bak and resets creds_ok=0; the boot path increments an
# unconfirmed-boot counter and, after `credsShouldFallback` (threshold 3), rolls back to
# the backup. This test drives the emulator's SURVIVAL PLANE through:
#   1. good creds        → reachable (safe-mode heartbeat) + creds confirmed
#   2. push `s3 <host> <BADKEY>` → applied, channel now severed (bad key ⇒ 403, no HB)
#   3. reboot repeatedly, NO further commands → assert it AUTO-ROLLS-BACK and becomes
#      reachable again within a few boots.
# PASS ⇒ the self-sever is self-healing (my earlier "no recovery" was leftover NVS).
# FAIL ⇒ a real remote-mgmt gap; fix cliSetS3/credsShouldFallback.
set -u
TARGET=emulator
source "$(dirname "$0")/e2e_lib.sh"
CMD_HOST="${API_HOST:-disw6oxjed.execute-api.us-west-2.amazonaws.com}"
DEVICE=EMU_S3REC
BADKEY=BADKEYbadkeyBADKEYbadkeyBADKEYbadkey00
L=/tmp/s3rec.log
PASS=0; FAIL=0

( cd "$FW_DIR" && ~/.local/bin/pio run -e emulator 2>&1 | tail -1 )
[ -x "$EMU" ] || { echo "FATAL: emu missing"; exit 1; }
rm -f "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_modem.dat"
for k in airbridge.cmd ack.json; do aws s3 rm "s3://$BUCKET/commands/$DEVICE/$k" >/dev/null 2>&1; done
aws s3 rm "s3://$BUCKET/heartbeat/$DEVICE.json" >/dev/null 2>&1
export EMU_SAFE_MODE=1

hb_ts() { aws s3 ls "s3://$BUCKET/heartbeat/$DEVICE.json" 2>/dev/null | awk '{print $1" "$2}'; }

# Boot the survival plane; return 0 if a safe-mode heartbeat is logged within <budget>s.
# Holds <hold>s after (so cred-confirm / counter logic settles), then power-cuts.
boot_cycle() { # <budget_s> <hold_s>
    : > "$L"; ( cd "$FW_DIR" && exec "$EMU" "$DEVICE" >>"$L" 2>&1 ) & local pid=$! ok=1 i
    for i in $(seq 1 "$1"); do
        grep -qa "\[HB\] posted mode=safe" "$L" && { ok=0; break; }
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
    done
    sleep "${2:-2}"
    kill -9 "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
    return $ok
}

log "── 1. good creds → reachable + confirmed ──"
if boot_cycle 45 4; then log "PASS: reachable on good creds"; PASS=$((PASS+1)); else log "FAIL: not reachable on good creds"; FAIL=$((FAIL+1)); fi
good_ts=$(hb_ts); log "   good heartbeat ts=$good_ts"

log "── 2. push bad s3 (good host, BAD key) ──"
aws s3 rm "s3://$BUCKET/commands/$DEVICE/ack.json" >/dev/null 2>&1
printf 's3 %s %s\n' "$CMD_HOST" "$BADKEY" | aws s3 cp - "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
boot_cycle 45 6
a=$(aws s3 cp "s3://$BUCKET/commands/$DEVICE/ack.json" - 2>/dev/null)
echo "$a" | grep -qa '"ran":true' && { log "PASS: bad s3 applied (ack ran=true)"; PASS=$((PASS+1)); } || log "   WARN: s3 ack not observed ($a)"
aws s3 rm "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
sev_ts=$(hb_ts); log "   heartbeat ts after bad s3 = $sev_ts (should stop advancing)"

log "── 3. reboot repeatedly, NO commands — expect auto-rollback ──"
base=$(hb_ts); recovered=0
for b in $(seq 1 8); do
    boot_cycle 45 3
    now=$(hb_ts)
    if [ -n "$now" ] && [ "$now" != "$base" ]; then
        log "   boot $b: ✓ RECOVERED — fresh heartbeat ($now)"; recovered=$b; break
    fi
    grep -qa "rolled back to previous known-good" "$L" && log "   boot $b: (rollback logged, waiting for HB)"
    log "   boot $b: still dark (bad creds persist)"
done
if [ "$recovered" -gt 0 ]; then
    log "PASS: auto-recovered from bad-s3 self-sever after $recovered reboot(s) — machinery works"; PASS=$((PASS+1))
else
    log "FAIL: NEVER recovered from bad-s3 in 8 reboots — REAL remote-mgmt gap, fix needed"; FAIL=$((FAIL+1))
fi

for k in airbridge.cmd ack.json; do aws s3 rm "s3://$BUCKET/commands/$DEVICE/$k" >/dev/null 2>&1; done
log ""
log "═══ S3 self-sever recovery: PASS=$PASS FAIL=$FAIL ═══"
exit "$FAIL"

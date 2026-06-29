#!/bin/bash
# Remote command channel e2e — the emulator fetches commands from the REAL test
# Lambda/S3 over (simulated) cellular, runs them through the SAME shared executor as a
# USB airbridge.cmd, and acks. Proves the cellular command-and-control channel end to
# end, including remote recovery of a WEDGED (corrupt-SD) unit with zero USB/physical
# access — the exact recovery the airbridge.cmd magic-file channel cannot do once a unit
# is deployed (the aircraft, not a laptop, is the USB host).
#
# Usage: scripts/cmd_channel_e2e.sh
set -u
TARGET="emulator"
source "$(dirname "$0")/e2e_lib.sh"
PASS=0; FAIL=0
ok(){ echo "  PASS: $*"; PASS=$((PASS+1)); }
no(){ echo "  FAIL: $*"; FAIL=$((FAIL+1)); }
EMU="$FW_DIR/.pio/build/emulator/program"

run_emu_until() {  # device, marker, timeout_s  → launch emu, wait for marker, kill
    local dev="$1" marker="$2" t="$3" i
    : > /tmp/emu_e2e.log
    ( cd "$FW_DIR" && "$EMU" "$dev" >>/tmp/emu_e2e.log 2>&1 ) &
    EMU_PID=$!
    for i in $(seq 1 "$t"); do
        grep -qa "$marker" /tmp/emu_e2e.log 2>/dev/null && break
        kill -0 "$EMU_PID" 2>/dev/null || break
        sleep 1
    done
    kill "$EMU_PID" 2>/dev/null; sleep 1; kill -9 "$EMU_PID" 2>/dev/null
}

# ── Scenario 1: remote config change (compress on) + acceptance ack ───────────
DEV1="EMU_E2E_cmd$(date +%H%M%S 2>/dev/null || echo 1)"
log "── Scenario 1: remote 'compress on'  device=$DEV1 ──"
aws s3 rm "s3://$BUCKET/commands/$DEV1/ack.json" >/dev/null 2>&1
printf 'compress on\n' | aws s3 cp - "s3://$BUCKET/commands/$DEV1/airbridge.cmd" >/dev/null 2>&1
unset EMU_SD_BLOCK EMU_SD_CORRUPT_AFTER_MS
run_emu_until "$DEV1" "compress ON" 90
grep -qa "remote command received" /tmp/emu_e2e.log && ok "command fetched over cellular" || no "command not fetched"
grep -qa "compress ON" /tmp/emu_e2e.log && ok "directive executed (identical to USB path)" || no "directive not executed"
aws s3 ls "s3://$BUCKET/commands/$DEV1/airbridge.cmd" >/dev/null 2>&1 && no "command NOT one-shot deleted" || ok "command one-shot deleted = received"
A1=$(aws s3 cp "s3://$BUCKET/commands/$DEV1/ack.json" - 2>/dev/null)
echo "$A1" | grep -qE '"ran":true' && ok "device ACKed execution: $A1" || no "no ack ($A1)"

# ── Scenario 2: REMOTE RECOVERY of a corrupt SD via format_sd (the headline) ──
DEV2="EMU_E2E_fmt$(date +%H%M%S 2>/dev/null || echo 2)"
log "── Scenario 2: remote format_sd recovers a corrupt SD  device=$DEV2 ──"
aws s3 rm "s3://$BUCKET/commands/$DEV2/ack.json" >/dev/null 2>&1
printf 'format_sd\n' | aws s3 cp - "s3://$BUCKET/commands/$DEV2/airbridge.cmd" >/dev/null 2>&1
export EMU_SD_BLOCK=1 EMU_SD_CORRUPT_AFTER_MS=9000   # corrupt the SD ~9s in
run_emu_until "$DEV2" "recovered (remote format_sd)" 60
unset EMU_SD_BLOCK EMU_SD_CORRUPT_AFTER_MS
grep -qa "corruption injected" /tmp/emu_e2e.log && ok "SD corrupted (unit wedged)" || no "SD not corrupted"
grep -qa "format_sd — reformatting (remote)" /tmp/emu_e2e.log && ok "remote format_sd executed on the corrupt SD over cellular" || no "remote format not executed"
grep -qa "recovered (remote format_sd)" /tmp/emu_e2e.log && ok "device RECOVERED via remote format — zero USB" || no "no recovery"
A2=$(aws s3 cp "s3://$BUCKET/commands/$DEV2/ack.json" - 2>/dev/null)
echo "$A2" | grep -qE '"format":true' && ok "device ACKed the format: $A2" || no "no format ack ($A2)"

echo "═══════════════════════════════════════════════════════════════"
echo "  Remote command channel e2e:  PASS=$PASS  FAIL=$FAIL"
echo "═══════════════════════════════════════════════════════════════"
exit "$FAIL"

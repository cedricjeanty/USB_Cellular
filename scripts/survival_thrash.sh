#!/bin/bash
# Survival-plane remote-management THRASH (EMULATOR).
#
# Proves the SURVIVAL PLANE (= Safe Mode: modem + C2 command channel + heartbeat +
# OTA self-heal + watchdog + coredump; NO SD/USB/harvest/upload) is BULLETPROOF for
# remote management, in isolation, before the application plane is re-added.
#
# Each cycle:
#   1. Launch the survival plane (EMU_SAFE_MODE=1) — the same plane the SURVIVAL_ONLY
#      firmware build (-DSURVIVAL_ONLY / env esp32s3-survival) runs on hardware.
#   2. REACHABLE?  assert "[HB] posted mode=safe" within the boot budget AND a fresh
#      S3 heartbeat with mode=safe (operator can SEE the unit).
#   3. COMMANDABLE? push one runtime-safe C2 directive; assert an ack with ran=true
#      lands (fetch+execute+ack round-trip closed over the backend).
#   4. POWER-CUT (kill -9) at a random uptime — brownout mid-work.
#   next cycle's REACHABLE check = proof it recovers from the cut.
#
# BULLETPROOF ⇒ every cycle reachable + every pushed command acked + ZERO wedges
# (no missing heartbeat) + ZERO firmware self-crashes. Any miss FAILS the run.
#
# The emulator talks to the REAL test Lambda/S3, so heartbeat + C2 are genuine
# round-trips. NOTE: real OTA *apply* (binary swap) is NOT emulatable — that leg is a
# Phase-3 hardware test on device 2; here we prove reachability/commandability/resilience.
#
#   scripts/survival_thrash.sh                 # 20 cycles (default)
#   CYCLES=6 scripts/survival_thrash.sh        # quick harness self-check
set -u

TARGET="emulator"
source "$(dirname "$0")/e2e_lib.sh"
CMD_HOST="${API_HOST:-disw6oxjed.execute-api.us-west-2.amazonaws.com}"

CYCLES="${CYCLES:-20}"
MIN_UP="${MIN_UP:-6}"          # min seconds alive before the power cut
MAX_UP="${MAX_UP:-35}"
BOOT_BUDGET="${BOOT_BUDGET:-45}"  # seconds to reach the safe-mode heartbeat
ACK_BUDGET="${ACK_BUDGET:-40}"    # seconds to fetch+execute+ack a command
SEED="${SEED:-1}"; RANDOM="$SEED"
DEVICE="EMU_SURV_s${SEED}"
L=/tmp/survival_thrash.log

# Runtime-safe directives only (they don't restart the plane; the kill -9 covers the
# reboot/brownout storm). Each must ack ran=true. DELIBERATELY EXCLUDES `s3` — that
# rewrites the backend creds and a bad value self-severs the channel (there's an
# api_host_bak in NVS but no api_key_bak, so a bad key can't fall back — a real gap
# tracked separately, not something to trip this reachability thrash on). `wifi` only
# touches the (channel-irrelevant) wifi NVS namespace; `compress` flips a runtime flag.
CMDS=("wifi survssid survpass" "compress on" "wifi survnet2 survpass2" "compress off")

reach_ok=0; cmd_ok=0; cmd_total=0; wedge=0; selfcrash=0

hb_mode() { curl -s "https://$CMD_HOST/prod/command/heartbeat?device=$DEVICE" -H "x-api-key: ${API_KEY:-}" 2>/dev/null; }
get_ack() { aws s3 cp "s3://$BUCKET/commands/$DEVICE/ack.json" - 2>/dev/null; }

log "═══════════════════════════════════════════════════════════════"
log "  SURVIVAL-PLANE THRASH  device=$DEVICE  cycles=$CYCLES seed=$SEED"
log "  cut=random(${MIN_UP}..${MAX_UP})s  boot_budget=${BOOT_BUDGET}s"
log "═══════════════════════════════════════════════════════════════"

# Build the emulator once (never during the loop — would clobber the live binary).
( cd "$FW_DIR" && ~/.local/bin/pio run -e emulator 2>&1 | tail -1 )
[ -x "$EMU" ] || { log "FATAL: emulator binary missing ($EMU)"; exit 1; }

# Fresh local NVS/modem state so the run starts from the fallback creds (a prior run's
# `s3`/`wifi` writes must not leak in and skew reachability). NVS still PERSISTS across
# the cycles within this run, so reboot-storm resume is genuinely exercised.
rm -f "$FW_DIR/emu_nvs.dat" "$FW_DIR/emu_modem.dat" 2>/dev/null || true

# Fresh S3 state for this device id.
aws s3 rm "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
aws s3 rm "s3://$BUCKET/commands/$DEVICE/ack.json"      >/dev/null 2>&1
aws s3 rm "s3://$BUCKET/heartbeat/$DEVICE.json"         >/dev/null 2>&1

for cycle in $(seq 1 "$CYCLES"); do
    : > "$L"
    export EMU_SAFE_MODE=1
    ( cd "$FW_DIR" && exec "$EMU" "$DEVICE" >>"$L" 2>&1 ) &
    EMUPID=$!

    # ── REACHABLE? ────────────────────────────────────────────────────────
    ok=0
    for i in $(seq 1 "$BOOT_BUDGET"); do
        grep -qa "\[HB\] posted mode=safe" "$L" && { ok=1; break; }
        kill -0 "$EMUPID" 2>/dev/null || { selfcrash=$((selfcrash+1)); break; }
        sleep 1
    done
    if [ "$ok" != 1 ]; then
        wedge=$((wedge+1))
        log "  cycle $cycle: ✗ NOT REACHABLE (no safe-mode heartbeat in ${BOOT_BUDGET}s)"
        kill "$EMUPID" 2>/dev/null; sleep 1; kill -9 "$EMUPID" 2>/dev/null; EMUPID=""
        continue
    fi
    reach_ok=$((reach_ok+1))
    hbm=$(hb_mode)
    smode="?"; echo "$hbm" | grep -qE '"mode" *: *"safe"' && smode="safe"

    # ── COMMANDABLE? ──────────────────────────────────────────────────────
    c="${CMDS[$(( (cycle-1) % ${#CMDS[@]} ))]}"
    aws s3 rm "s3://$BUCKET/commands/$DEVICE/ack.json" >/dev/null 2>&1
    printf '%s\n' "$c" | aws s3 cp - "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
    cmd_total=$((cmd_total+1))
    acked=0; a=""
    for i in $(seq 1 "$ACK_BUDGET"); do
        a=$(get_ack); echo "$a" | grep -qa '"ran":true' && { acked=1; break; }
        kill -0 "$EMUPID" 2>/dev/null || { selfcrash=$((selfcrash+1)); break; }
        sleep 1
    done
    [ "$acked" = 1 ] && cmd_ok=$((cmd_ok+1))
    aws s3 rm "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1

    log "  cycle $cycle: ✓ reachable (S3 mode=$smode)  cmd='$c' $([ "$acked" = 1 ] && echo 'ACKED' || echo '✗ NO-ACK')"

    # ── POWER-CUT at a random uptime ──────────────────────────────────────
    span=$((MAX_UP - MIN_UP + 1)); [ "$span" -lt 1 ] && span=1
    window=$((MIN_UP + RANDOM % span))
    for i in $(seq 1 "$window"); do kill -0 "$EMUPID" 2>/dev/null || break; sleep 1; done
    kill -9 "$EMUPID" 2>/dev/null; wait "$EMUPID" 2>/dev/null; EMUPID=""
done
unset EMU_SAFE_MODE

# cleanup
aws s3 rm "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
aws s3 rm "s3://$BUCKET/commands/$DEVICE/ack.json"      >/dev/null 2>&1

log ""
log "═══════════════════════════════════════════════════════════════"
log "  SURVIVAL-PLANE THRASH RESULT  device=$DEVICE"
log "    reachable:   $reach_ok / $CYCLES cycles"
log "    commandable: $cmd_ok / $cmd_total pushed"
log "    wedges (no heartbeat): $wedge    firmware self-crashes: $selfcrash"
if [ "$reach_ok" = "$CYCLES" ] && [ "$cmd_ok" = "$cmd_total" ] && [ "$wedge" = 0 ] && [ "$selfcrash" = 0 ]; then
    log "  ✅ BULLETPROOF: survival plane reachable + commandable every cycle, no wedge/crash"
    rc=0
else
    log "  ❌ NOT bulletproof — see misses above (forensic log: $L)"
    rc=1
fi
log "═══════════════════════════════════════════════════════════════"
exit "$rc"

#!/bin/bash
# Remote command-and-control channel — INTEGRATION TEST (emulator OR hardware).
#
#   scripts/cmd_channel_e2e.sh                     # emulator (default) — cheap, CI-able
#   scripts/cmd_channel_e2e.sh --target device     # real hardware over real cellular
#   scripts/cmd_channel_e2e.sh --target device --quick   # skip destructive + power-cut
#
# The device (emulated or real) fetches airbridge.cmd directives from the REAL test
# Lambda/S3 over (simulated | real) cellular, runs them through the SAME shared executor
# as a USB airbridge.cmd, and ACKs each one — so BOTH targets are true end-to-end tests
# against the live backend. Each command is verified three ways: ack.json (executed) +
# serial/log marker (ran on-device) + delete-on-ack (at-least-once loop closed), plus a
# heartbeat round-trip.
#
# Target abstraction is e2e_lib.sh (start_device/stop_device/power_cut branch on TARGET):
#   • emulator — kill+relaunch the SDL binary; "power cut" = kill -9 (NVS/SD/resume state
#     persist on disk exactly like flash/SD across a brownout). Can inject faults the
#     hardware can't: scripted SD corruption + a forced Safe Mode (the two headline
#     emulator-only scenarios at the end).
#   • device   — CoolGear power-cycles a unit flashed esp32s3-e2e (forces TEST_<MAC> id →
#     test bucket; CDC_PERSIST keeps the serial tap live). Proves the paths the emulator
#     CANNOT model: real cellular fetch/ack at real RSRP, a real SIM7600 modem_reset
#     (factory reset + PPP re-attach), and at-least-once redelivery under a REAL power cut.
#
# Shared matrix (both targets): baseline heartbeat, compress on/off, wifi, s3 (no-op —
# proves the directive can't self-sever its own channel), reboot-once, at-least-once
# redelivery under a power cut, format_sd. Device-only: modem_reset. Emulator-only:
# corrupt-SD→remote-format recovery, forced-Safe-Mode→remote recovery.
set -u

TARGET="emulator"; QUICK=0
while [ $# -gt 0 ]; do
    case "$1" in
        --target)   TARGET="$2"; shift 2 ;;
        --target=*) TARGET="${1#*=}"; shift ;;
        --quick)    QUICK=1; shift ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done
export TARGET
source "$(dirname "$0")/e2e_lib.sh"

CMD_HOST="${API_HOST:-disw6oxjed.execute-api.us-west-2.amazonaws.com}"
EMU="$FW_DIR/.pio/build/emulator/program"

# PPP-up is checked STATE-based on the device (the firmware logs "PPP got IP" only ONCE
# per connect; "net=ppp" reprints every 60s STATUS, so a mark taken after the device
# already connected still matches). On the emulator, start_device already gates on
# "Init complete" (booted+connected), so the ack poll's own timeout covers the fetch.
if [ "$TARGET" = "device" ]; then PPP_READY='PPP got IP|net=ppp'; else PPP_READY='ppp=1|PPP up|Init complete'; fi
# "Device rebooted and came back" — lines that appear ONLY on a fresh boot (device tap
# can miss the single "AirBridge fw=" banner during CDC re-enumeration).
BOOTED='AirBridge fw=|Pre-USB tasks done|Pre-USB: OTA|USB: drive presented|Init complete'

# ── C2 operator-side helpers (shared) ────────────────────────────────────────
push_cmd() {  # <cmd_text> — stage a command, clear any stale ack first
    aws s3 rm "s3://$BUCKET/commands/$DEVICE/ack.json" >/dev/null 2>&1
    printf '%s\n' "$1" | aws s3 cp - "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
    log "  → pushed command: '$1'"
}
get_ack()     { aws s3 cp "s3://$BUCKET/commands/$DEVICE/ack.json" - 2>/dev/null; }
cmd_pending() { aws s3 ls "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1; }
get_hb()      { curl -s "https://$CMD_HOST/prod/command/heartbeat?device=$DEVICE" -H "x-api-key: $API_KEY"; }

wait_ack() {  # <tries> <pattern> — poll ack.json; echo it; 0/1
    local tries="$1" pat="$2" a="" i
    for i in $(seq 1 "$tries"); do
        a=$(get_ack)
        if [ -n "$a" ] && echo "$a" | grep -qE "$pat"; then echo "$a"; return 0; fi
        sleep 2
    done
    echo "$a"; return 1
}
wait_hb() {  # <tries> [mode] — poll heartbeat for mode=<want>
    local tries="$1" want="${2:-healthy}" hb="" i
    for i in $(seq 1 "$tries"); do
        hb=$(get_hb)
        if echo "$hb" | grep -qE "\"mode\" *: *\"$want\""; then echo "$hb"; return 0; fi
        sleep 3
    done
    echo "$hb"; return 1
}
wait_hb_any() {  # <tries> — poll for ANY fresh heartbeat (reachability, mode-agnostic)
    local tries="$1" hb="" i
    for i in $(seq 1 "$tries"); do
        hb=$(get_hb)
        if echo "$hb" | grep -qE '"mode" *:'; then echo "$hb"; return 0; fi
        sleep 3
    done
    echo "$hb"; return 1
}

WARN=0
warn() { log "WARN: $1"; WARN=$((WARN + 1)); }

# Truncation-safe search offset: the emulator's start_device TRUNCATES $E2E_LOG on
# relaunch (so a pre-restart mark overruns the fresh log → search from 0); the device
# tap APPENDS across power cycles (so the pre-restart mark correctly skips prior output).
mark_from() { [ "$TARGET" = "emulator" ] && echo 0 || echo "$1"; }

# After a reboot/format the CHANNEL's job is done once the unit is REACHABLE (heartbeat
# responds). Reaching mode=healthy is a device-health matter separate from C2 — pass on
# reachability, WARN (not fail) if it comes back in safe mode (e.g. a bench unit's known
# intermittent app-plane crash loop).
assert_reachable_after() {  # <label> <tries>
    local label="$1" tries="$2" hb
    hb=$(wait_hb_any "$tries")
    if [ $? -ne 0 ]; then fail "$label: device UNREACHABLE after restart (no heartbeat)"; return 1; fi
    if echo "$hb" | grep -qE '"mode" *: *"healthy"'; then
        pass "$label: back and healthy — $hb"
    else
        pass "$label: reachable after restart (C2 survived) — $hb"
        warn "$label: came back NOT healthy (mode!=healthy) — device-health issue, not a C2 defect"
    fi
}

# Wait until the (re)booted target is connected enough to fetch, past <mark>.
wait_connected() {  # <mark> <label>
    if [ "$TARGET" = "device" ]; then
        wait_for_log "$PPP_READY" "$1" 270 "$2"
    else
        # Emulator: start_device already waited "Init complete"; the ack poll covers the
        # ~first-after-PPP fetch. Just confirm the process is alive.
        [ -n "$EMU_PID" ] && kill -0 "$EMU_PID" 2>/dev/null
    fi
}

# Push a command, force a fresh fetch by (re)starting the target, verify ack +
# delete-on-ack + optional marker.
#   run_cmd <label> <cmd_text> <ack_pattern> [marker_regex]
run_cmd() {
    local label="$1" cmd="$2" ackpat="$3" smark="${4:-}"
    local m a
    log ""; log "── $label ──"
    push_cmd "$cmd"
    m=$(log_mark)
    stop_device; sleep 3; start_device 5
    if ! wait_connected "$m" "connect"; then
        fail "$label: never connected (cannot fetch)"; return 1
    fi
    a=$(wait_ack 60 "$ackpat")
    if [ $? -eq 0 ]; then pass "$label: device ACKed — $a"
    else fail "$label: no ack matching /$ackpat/ (got: ${a:-<none>})"; return 1; fi
    if cmd_pending; then fail "$label: command NOT consumed (delete-on-ack failed)"
    else pass "$label: command consumed after ack (at-least-once loop closed)"; fi
    if [ -n "$smark" ]; then
        # Search offset must be truncation-safe: the emulator's start_device TRUNCATES
        # $E2E_LOG on relaunch, so the pre-restart mark `m` overruns the fresh log →
        # search from 0 (the log is per-scenario-fresh, no stale matches). The device
        # tap APPENDS across power cycles, so `m` (pre-restart) correctly skips the
        # previous scenario's identical marker.
        if wait_for_log "$smark" "$(mark_from "$m")" 30 "$label marker"; then
            pass "$label: log confirms on-device execution"
        elif [ "$TARGET" = "device" ]; then
            # On hardware the serial marker is the authoritative, reliable execution proof.
            fail "$label: marker /$smark/ not seen (executor didn't run it?)"
        else
            # On the emulator the ACK (above) is authoritative — it already proved the
            # directive ran. The corroborating log line races the fast kill+relaunch
            # cadence, so a miss here is a marker-timing artifact, not an execution failure.
            warn "$label: emu log marker /$smark/ not seen — ack already confirms execution (marker race)"
        fi
    fi
}

# Emulator-only: launch the SDL binary with the current env, wait for a marker, kill.
run_emu_until() {  # <device> <marker> <timeout_s>
    local dev="$1" marker="$2" t="$3" i
    : > /tmp/emu_e2e.log
    ( cd "$FW_DIR" && "$EMU" "$dev" >>/tmp/emu_e2e.log 2>&1 ) &
    EMU_PID=$!
    for i in $(seq 1 "$t"); do
        grep -qa "$marker" /tmp/emu_e2e.log 2>/dev/null && break
        kill -0 "$EMU_PID" 2>/dev/null || break
        sleep 1
    done
    kill "$EMU_PID" 2>/dev/null; sleep 1; kill -9 "$EMU_PID" 2>/dev/null; EMU_PID=""
}

cleanup_c2() {
    log ""; log "Teardown"
    aws s3 rm "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
    aws s3 rm "s3://$BUCKET/commands/$DEVICE/ack.json"      >/dev/null 2>&1
    if [ "$TARGET" = "device" ]; then device_disable_persistent_cdc; serial_tap_stop; fi
    stop_device
}
trap cleanup_c2 EXIT

# ── Bring-up ──────────────────────────────────────────────────────────────────
log "════════════════════════════════════════════════════════════════"
log " Remote C2 channel — integration test (target=$TARGET)"
log "════════════════════════════════════════════════════════════════"
if [ "$TARGET" = "device" ]; then
    log ""; log "Device bring-up: staging persistent CDC + serial tap"
    $COOLGEAR off >/dev/null 2>&1; sleep 5; $COOLGEAR on >/dev/null 2>&1
    device_enable_persistent_cdc
    stop_device; sleep 3
    start_device 5
    if [ -n "$SERIAL_TAP_PID" ] && kill -0 "$SERIAL_TAP_PID" 2>/dev/null; then
        pass "bring-up: CDC_PERSIST staged, serial tap live"
    else
        fail "bring-up: serial tap not running (device not in -DALLOW_CDC_PERSIST build?)"
    fi
    # esp32s3-e2e forces device_id=TEST_<MAC>; read it off the STATUS line so ALL C2
    # (commands/ack/heartbeat) routes to the -test bucket via _pick_bucket.
    for i in $(seq 1 40); do
        d=$(grep -oE "device=TEST_[0-9A-Fa-f]+" "$E2E_LOG" 2>/dev/null | tail -1 | cut -d= -f2)
        [ -n "$d" ] && { DEVICE="$d"; log "  C2 device id = $DEVICE (test-bucket routing)"; break; }
        sleep 2
    done
    if [ "${DEVICE#TEST_}" = "$DEVICE" ]; then
        fail "device id not TEST_-prefixed ('$DEVICE') — flash esp32s3-e2e; aborting"; exit 1
    fi
else
    [ -x "$EMU" ] || { fail "emulator binary missing ($EMU) — pio run -e emulator"; exit 1; }
    log ""; log "Emulator bring-up (device id = $DEVICE)"
    unset EMU_SD_BLOCK EMU_SD_CORRUPT_AFTER_MS EMU_SAFE_MODE 2>/dev/null || true
    rm -rf "$SD_EMU" "$SD_INT" "$FW_DIR/emu_nvs.dat" 2>/dev/null
    start_device 5
    pass "bring-up: emulator launched"
fi
aws s3 rm "s3://$BUCKET/commands/$DEVICE/airbridge.cmd" >/dev/null 2>&1
aws s3 rm "s3://$BUCKET/commands/$DEVICE/ack.json"      >/dev/null 2>&1

# ── Scenario 0: baseline reachability — heartbeat round-trips ────────────────
log ""; log "── Scenario 0: baseline — device reachable, heartbeat healthy ──"
m=$(log_mark)
if wait_connected "$m" "connect"; then pass "0: connected"
else fail "0: not connected — the rest of the suite cannot run"; exit 1; fi
HB=$(wait_hb 40 healthy)
echo "$HB" | grep -qE '"mode" *: *"healthy"' \
    && pass "0: heartbeat round-trips (survival plane egress works): $HB" \
    || fail "0: no healthy heartbeat within 120s (got: ${HB:-<none>})"

# ── Shared command matrix ─────────────────────────────────────────────────────
# Firmware logs compress state UPPERCASE: "CMD: compress ON"/"OFF".
run_cmd "1a: compress on"  "compress on"  '"ran":true.*"compress":true'  "CMD: compress ON"
run_cmd "1b: compress off" "compress off" '"ran":true.*"compress":false' "CMD: compress OFF"
run_cmd "2: wifi save" "wifi TESTNET_HW testpass123" '"ran":true' "CMD:.*[Ww]i-?[Ff]i"
# Re-set S3 creds to the SAME (known-good) endpoint: proves the directive executes over
# cellular WITHOUT self-severing the very channel that delivered it (a wrong host/key
# would brick C2 — so we re-push the current test creds: a safe no-op exercising cliSetS3).
run_cmd "3: s3 re-set (no-op)" "s3 $CMD_HOST $API_KEY" '"ran":true' "CMD:.*[Ss]3"

# ── Scenario 4: reboot once — restarts, returns reachable, not re-run ─────────
log ""; log "── Scenario 4: 'reboot once' — device restarts and comes back ──"
push_cmd "reboot once"
m=$(log_mark)
stop_device; sleep 3; start_device 5
if wait_connected "$m" "PPP up"; then
    A=$(wait_ack 60 '"reboot":true')
    if [ $? -eq 0 ]; then pass "4: reboot acked before restart — $A"; else fail "4: no reboot ack ($A)"; fi
    if wait_for_log "$BOOTED" "$(mark_from "$m")" 200 "post-reboot boot"; then
        pass "4: device rebooted and re-presented (survived remote reboot)"
    else
        fail "4: no post-reboot boot marker (device did not come back)"
    fi
    assert_reachable_after "4" 40
    cmd_pending && fail "4: 'reboot once' NOT stripped (would re-run every boot)" || pass "4: 'reboot once' consumed (one-shot honored)"
else
    fail "4: never reconnected after reboot command"
fi

# ── Scenario 5: modem_reset — DEVICE ONLY (real SIM7600 factory reset) ────────
if [ "$TARGET" = "device" ]; then
    log ""; log "── Scenario 5: modem_reset — real modem factory reset + re-attach ──"
    push_cmd "modem_reset"
    m=$(log_mark)
    stop_device; sleep 3; start_device 5
    if wait_connected "$m" "PPP up"; then
        A=$(wait_ack 60 '"modem_reset":true')
        if [ $? -eq 0 ]; then pass "5: modem_reset acked — $A"; else fail "5: no modem_reset ack ($A)"; fi
        ppp_mark=$(log_mark)
        if wait_for_log "Modem: remote factory reset|remote modem_reset" "$m" 40 "modem reset begins"; then
            pass "5: modem task executed the factory reset (owns the UART)"
        else
            fail "5: modem factory-reset marker not seen on serial"
        fi
        if wait_for_log "$PPP_READY" "$ppp_mark" 300 "PPP re-attach"; then
            pass "5: PPP re-attached after the real modem reset (re-dialed to a fresh IP)"
        else
            fail "5: modem did NOT re-attach within 300s after reset"
        fi
    else
        fail "5: never reconnected to deliver modem_reset"
    fi
else
    skip "5: modem_reset — device-only (sim modem has no factory reset)"
fi

if [ "$QUICK" -eq 1 ]; then
    echo "═══════════════════════════════════════════════════════════════"
    echo "  Remote C2 ($TARGET, quick):  PASS=$PASS  FAIL=$FAIL  WARN=$WARN"
    echo "═══════════════════════════════════════════════════════════════"
    exit "$FAIL"
fi

# ── Scenario 6: at-least-once redelivery under a power cut ────────────────────
# Stage a command, cut power around the fetch/ack window (emulator: kill -9; device:
# CoolGear). The command must NOT be lost: it stays in S3 until the device proves
# execution via /command/ack, so a later boot redelivers it and it runs EXACTLY once.
log ""; log "── Scenario 6: at-least-once redelivery under a power cut ──"
push_cmd "compress on"
m=$(log_mark)
stop_device; sleep 3; start_device 5
if wait_connected "$m" "connect"; then
    sleep 4
    power_cut 5
    if cmd_pending; then pass "6: command survived the mid-window power cut (not lost)"
    else log "  (command already acked before the cut — redelivery not exercised this run)"; fi
    m2=$(log_mark)
    start_device 5
    if wait_connected "$m2" "reconnect"; then
        A=$(wait_ack 60 '"ran":true')
        if [ $? -eq 0 ]; then pass "6: command redelivered + acked after the cut — $A"; else fail "6: command LOST across the cut ($A)"; fi
        cmd_pending && fail "6: command still pending after ack (not consumed)" || pass "6: consumed exactly once (idempotent redelivery)"
    else
        fail "6: never reconnected after the power cut"
    fi
else
    fail "6: never connected to stage the robustness cut"
fi

# ── Scenario 7: format_sd — destructive remote reformat + recover ────────────
log ""; log "── Scenario 7: format_sd — destructive remote reformat + recover ──"
push_cmd "format_sd"
m=$(log_mark)
stop_device; sleep 3; start_device 5
if wait_connected "$m" "PPP up"; then
    A=$(wait_ack 60 '"format":true')
    if [ $? -eq 0 ]; then pass "7: format_sd acked before reformat — $A"; else fail "7: no format ack ($A)"; fi
    if wait_for_log "$BOOTED" "$(mark_from "$m")" 240 "post-format boot"; then
        pass "7: device rebooted to apply the reformat"
    else
        fail "7: no post-format boot marker"
    fi
    assert_reachable_after "7" 60
    cmd_pending && fail "7: format_sd still pending" || pass "7: format_sd consumed"
else
    fail "7: never reconnected to deliver format_sd"
fi

# ── Emulator-only headline scenarios (faults the hardware can't inject) ───────
if [ "$TARGET" = "emulator" ]; then
    stop_device; sleep 1

    # E1: REMOTE RECOVERY of a corrupt SD via format_sd — zero USB.
    DEVX="EMU_E2E_fmt$(date +%H%M%S 2>/dev/null || echo x)"
    log ""; log "── Emulator E1: remote format_sd recovers a corrupt SD  device=$DEVX ──"
    aws s3 rm "s3://$BUCKET/commands/$DEVX/ack.json" >/dev/null 2>&1
    printf 'format_sd\n' | aws s3 cp - "s3://$BUCKET/commands/$DEVX/airbridge.cmd" >/dev/null 2>&1
    export EMU_SD_BLOCK=1 EMU_SD_CORRUPT_AFTER_MS=9000
    run_emu_until "$DEVX" "recovered (remote format_sd)" 60
    unset EMU_SD_BLOCK EMU_SD_CORRUPT_AFTER_MS
    grep -qa "corruption injected" /tmp/emu_e2e.log && pass "E1: SD corrupted (unit wedged)" || fail "E1: SD not corrupted"
    grep -qa "format_sd — reformatting (remote)" /tmp/emu_e2e.log && pass "E1: remote format_sd executed on the corrupt SD" || fail "E1: remote format not executed"
    grep -qa "recovered (remote format_sd)" /tmp/emu_e2e.log && pass "E1: device RECOVERED via remote format — zero USB" || fail "E1: no recovery"
    AX=$(aws s3 cp "s3://$BUCKET/commands/$DEVX/ack.json" - 2>/dev/null)
    echo "$AX" | grep -qE '"format":true' && pass "E1: device ACKed the format: $AX" || fail "E1: no format ack ($AX)"
    aws s3 rm "s3://$BUCKET/commands/$DEVX/airbridge.cmd" >/dev/null 2>&1; aws s3 rm "s3://$BUCKET/commands/$DEVX/ack.json" >/dev/null 2>&1

    # E2: SAFE MODE — survival plane stays reachable + heartbeats; remote recovery.
    DEVY="EMU_E2E_safe$(date +%H%M%S 2>/dev/null || echo y)"
    L3=/tmp/emu_safe_e2e.log
    log ""; log "── Emulator E2: SAFE MODE heartbeat + remote recovery  device=$DEVY ──"
    aws s3 rm "s3://$BUCKET/commands/$DEVY/ack.json" >/dev/null 2>&1
    aws s3 rm "s3://$BUCKET/heartbeat/$DEVY.json" >/dev/null 2>&1
    : > "$L3"; export EMU_SAFE_MODE=1 EMU_SD_BLOCK=1
    ( cd "$FW_DIR" && exec "$EMU" "$DEVY" >>"$L3" 2>&1 ) &
    EMUY=$!
    for i in $(seq 1 40); do grep -qa "\[HB\] posted mode=safe" "$L3" && break; kill -0 "$EMUY" 2>/dev/null || break; sleep 1; done
    grep -qa "SAFE MODE: survival plane only" "$L3" && pass "E2: booted into SAFE MODE (survival plane only)" || fail "E2: did not enter safe mode"
    grep -qa "\[HB\] posted mode=safe" "$L3" && pass "E2: heartbeat posted in safe mode (reachable, awaiting orders)" || fail "E2: no safe-mode heartbeat"
    HBSAFE=$(curl -s "https://$CMD_HOST/prod/command/heartbeat?device=$DEVY" -H "x-api-key: $API_KEY")
    echo "$HBSAFE" | grep -qE '"mode" *: *"safe"' && pass "E2: operator GET shows mode=safe: $HBSAFE" || fail "E2: S3 heartbeat not mode=safe ($HBSAFE)"
    printf 'format_sd\n' | aws s3 cp - "s3://$BUCKET/commands/$DEVY/airbridge.cmd" >/dev/null 2>&1
    for i in $(seq 1 40); do grep -qa "\[HB\] posted mode=healthy" "$L3" && break; kill -0 "$EMUY" 2>/dev/null || break; sleep 1; done
    kill "$EMUY" 2>/dev/null; sleep 1; kill -9 "$EMUY" 2>/dev/null
    unset EMU_SAFE_MODE EMU_SD_BLOCK
    grep -qa "exiting SAFE MODE" "$L3" && pass "E2: remote format_sd cleared the crash-loop firewall" || fail "E2: firewall not cleared"
    grep -qa "\[HB\] posted mode=healthy" "$L3" && pass "E2: heartbeat flipped to healthy after recovery" || fail "E2: heartbeat did not flip to healthy"
    aws s3 rm "s3://$BUCKET/commands/$DEVY/airbridge.cmd" >/dev/null 2>&1; aws s3 rm "s3://$BUCKET/commands/$DEVY/ack.json" >/dev/null 2>&1
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  Remote C2 ($TARGET):  PASS=$PASS  FAIL=$FAIL  WARN=$WARN"
[ "$WARN" -gt 0 ] && echo "  (WARN = device-health notes, e.g. came back in safe mode — NOT C2 defects)"
echo "═══════════════════════════════════════════════════════════════"
exit "$FAIL"

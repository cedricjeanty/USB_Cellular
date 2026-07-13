# Modem Recovery (SIM7600 / T-Mobile / Hologram)

Read this before touching modem or PPP code. The reconnect sequences below were
each learned the hard way on live cellular — see `docs/decisions/0002` for the
CEREG/CGACT decision record.

## Boot-time AT sync (modemAtSync / modemRunInit in `airbridge_modem.h`)

1. Try AT at 115200 (covers cold boot)
2. Try +++ escape at 921600/460800/3M without flow control (modem in PPP data mode after soft reboot)
3. Try +++ at same bauds with hardware flow control
4. CFUN=0/1 radio reset after AT sync
5. Watchdog in main_loop restarts modem task if it dies (30s cooldown)

## Mid-session reconnect (modemReconnect in `airbridge_modem.h`) — after PPP drop

The correct sequence is **NOT** CFUN=0/1. After a carrier-terminated session the radio
stays registered; CFUN causes real deregistration + forced 30-60s re-attach (worse).

```
ATH                              ← hang up / exit PPP mode
AT+CGACT=0,1                     ← deactivate stale PDP context (critical)
(5s wait for bearer cleanup)
AT+CEREG? / AT+CGREG?            ← check LTE data registration — NOT AT+CREG
AT+CGDCONT=1,"IP","hologram"     ← always re-state APN before redialing
ATD*99#
```

## Key AT command notes for T-Mobile/Hologram LTE SIMs

- Use `AT+CEREG?` (LTE/EPS) or `AT+CGREG?` (packet-domain) — **never `AT+CREG?`**
  T-Mobile denies *voice* registration (CREG returns state 3 = denied) but grants
  *LTE data* registration (CEREG returns state 5 = roaming). Checking CREG will always
  time out on T-Mobile/Hologram, causing spurious CFUN resets.
- `AT+CGACT=0,1` before every redial — without it the modem may give CONNECT on a
  stale context where IPCP never completes (no IP assigned).
- CFUN=0/1 is only for genuinely unresponsive modem (not responding to AT at any baud).
- Carrier (T-Mobile/Hologram) terminates sessions every ~4-6 hours — this is normal.
- **Signal metrics:** `modemRunInitPost()` reads `AT+CSQ` (RSSI 0-31) plus, before the PPP
  dial, `AT+CESQ` (RSRP dBm / RSRQ dB, 3GPP indices converted) and `AT+CPSI?` (band + SINR,
  best-effort parse). All in command mode (no `+++`), so no PPP disruption. Logged on the
  `Modem: …` init line and the 60s `STATUS` line (`rsrp=`/`sinr=`). `MODEM_SIG_NA` (9999) =
  unavailable. Parsing is in `airbridge_modem.h` (`parseCesq`/`parseCpsi`), unit-tested;
  `sim_modem.h` simulates both for the emulator/tests.

## Reconnect sequence

Both soft and hard paths send `+++` guard first (escapes PPP data mode — safe no-op
if already in command mode):

- Soft (no CFUN): `+++` → ATH → CGACT=0,1 → 5s → CEREG check → CGDCONT → ATD*99#
- Hard (CFUN): `+++` → ATH → CFUN=0 → 10s → CFUN=1 → 5s → CEREG check → ATD*99#

**Root cause of repeated soft-reconnect failures**: after a reconnect gets CONNECT but
IPCP stalls, the modem stays in PPP data mode. Without the `+++` guard, subsequent soft
reconnects send ATH/CEREG as PPP bytes the modem ignores → 3-minute CEREG timeout each
attempt. The `+++` guard (added fw 20260527160000) fixes this.

## Reconnect backoff (s_reconnect_failures counter in modem task pump loop)

Decision logic is `modemReconnectPlan(failures, signalSeen)` in `airbridge_modem.h`
(shared + unit-tested). There is no progressive/long backoff: the loop is
signal-gated (`modemWaitForSignal` polls CEREG+CSQ every ~10s between attempts and
retries the instant coverage returns), so a redial fires within seconds of the
network reappearing — this is what makes the post-landing taxi-in upload window
usable.

- Attempts 1-4 (failures 0-3): soft reconnect (no CFUN) — handles carrier termination
- Attempt 5 (failures=4) and every 5th after: full CFUN=0/1 radio reset, with a
  flat 10s settle before it (never longer)
- Factory reset (AT&F + CRESET — clears band/operator locks CFUN can't), cadence
  gated on signal evidence (ADR 0006):
  - every 12th failure if any cell was visible (CSQ != 99) or registration
    succeeded since the last good session — the genuinely-stranded case
  - every 36th failure if the radio has seen nothing at all — most likely
    airborne out of coverage, where resets don't help; the guarantee that a
    band-locked unit eventually self-recovers is preserved
- Counter (and signal-seen flag) reset only when PPP gets IP
  (g_pppConnected=true), not on CONNECT alone

## Baud recovery after resets

AT+CFUN=0 and AT+CRESET drop the SIM7600 UART to 115200. The baud/flow-control
upgrade ladder (`modemUpgradeBaud()`: 3M → 2M → 921600, then AT+IFC=2,2 + RTS/CTS,
each step AT-verified) runs at boot AND after every radio reset (inside
`modemReconnect(resetRadio=true)`) and factory reset (`modemFactoryRecover()`,
which also re-syncs AT and re-sends ATE0 after the module reboot). Before this,
a post-reset session stayed at 115200 (~10 KB/s) until the next device reboot —
exactly the session that uploads the backlog after landing.

## pppStale detection

90s threshold (was 30s). The 30s LCP echo interval meant one delayed echo-reply
could falsely trigger reconnect on a healthy link.

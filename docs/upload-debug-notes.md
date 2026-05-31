# Upload Debugging — State & Next Steps (compacted 2026-05-31)

Working branch: **`gap1-upload-dedup`**. Device: PCB `9C139EF3D3D8` via CoolGear (`scripts/coolgear.py`).

## RESOLVED — there is no large-file *upload-code* bug
Spent a long arc thinking the 22 MB upload had an SD-lock / multipart bug. It does not:
- **Emulator multipart works** to BOTH buckets. `EMU_E2E` → test bucket (E2E TEST 15, 10 MB,
  passes). Isolation run `SERIAL=EA999.HWDIAG DEVICE=HWDIAG000001 CONTENTION=0` → **prod**
  bucket, 8 MB file **landed intact** (`scripts/repro_sd_contention.sh`). Prod test data was
  cleaned up.
- **SD/MSC contention model** (`EMU_SD_CONTENTION=1`) completes a 12 MB multipart upload,
  `mscTimeouts=0`, no deadlock → shared lock discipline is sound.
- **On hardware**, `ULDBG` breadcrumbs showed the 22 MB upload *progressing* (parts stream
  per-MB, reads+writes), NOT hanging on an SD read. Real fixes found+kept on the branch:
  `Esp32Filesys::readdir` now sets `is_dir` (latent bug), upload task stack 16→32 KB.
- Conclusion: **gap1's upload de-dup is functionally validated.** Mergeable once the modem
  issue below is addressed.

## ROOT CAUSE of the "upload hang" — modem zombie-link, caused by our code
On hardware the parts *stream* (bytes into TLS) but never land in S3, then the device goes
silent for hours, all under **`AT+CSQ`=99 (no detectable signal) while PPP still reports
connected**. That is a zombie link: lwIP/PPP thinks it's up, the radio has actually dropped,
data goes nowhere. NOT a degraded tower — a code/state problem.

Why our code doesn't recover (the gap):
- Link-health = `pppStale` on `lastPppRxMs` (`main.cpp:2533–2545`): fires only when the modem
  goes **fully silent** for `staleMs` (90 s, or **120 s while `g_tlsActive`**). It does NOT
  detect "modem still answers LCP echoes but no real internet data flows." LCP echo replies
  keep `lastPppRxMs` fresh → `pppStale` never fires → upload wedges indefinitely.
- `IP_EVENT_PPP_LOST_IP` is the only other trigger, and ESP-IDF lwIP-PPP is documented to lose
  the connection *without* firing it (esp-protocols #562/#637).
- Supporting: SIMCom does **not recommend PPP** for SIM7600 data ("high-bandwidth module; use
  QMI"). PPPoS-over-UART is inherently fragile for sustained transfers.

## NEXT STEPS (emulate-first; hardware only to learn what to emulate)
1. **HW capture to understand the zombie state** (the device is in it now — good).
   Instrument the firmware to log, every ~15 s during a session: `AT+CSQ`, `AT+CEREG?`,
   `AT+CGACT?`, `AT+CGPADDR`, and bytes-since-last *non-LCP* PPP RX. Capture via CDC while a
   large upload wedges. Tells us: is the radio deregistered? PDP context dead? data mode stuck?
   (Querying needs a `+++` escape mid-session — do it carefully / only on suspected stall.)
2. **Model the zombie state in `sim_modem.h`**: PPP stays up but data stops flowing / radio
   deregisters (CEREG→0, CSQ→99) mid-session, optionally still answering LCP. So we can
   reproduce + test recovery locally.
3. **Fix (test in emulator):**
   - App-level **upload-progress watchdog**: if an in-flight upload makes no real S3 progress
     (no part completes / TLS write rate ~0) for N s, force a full reconnect — this keys on
     *actual data delivery*, the only reliable signal.
   - Smarter stale detection: count only **non-LCP** PPP RX toward `lastPppRxMs`; add a
     periodic real-reachability check (DNS/keepalive), not just PPP-layer liveness.
   - Robust multipart error handling: verify each part's PUT response/ETag; fail-fast + retry
     on a rejected/empty-ETag part instead of streaming into the void.
   - Durable option to evaluate: SIM7600 built-in TCP/IP (AT `NETOPEN`/`CIPOPEN`) or its
     native TLS, replacing PPPoS for uploads (big change; weigh later).
4. **Validate** recovery in the emulator against the modeled zombie state, then hardware-confirm.

## Key files
- `esp32/include/airbridge_modem.h` — `modemAtSync`/`modemRunInitPre/Post`/`modemReconnect`.
- `esp32/src/main.cpp` — `modemTask` pump loop, `pppStale` (~2533), reconnect backoff (~2576),
  PPP event handlers (~2118).
- `esp32/include/sim_modem.h` — SimModem (extend to inject zombie-link failure modes).
- `esp32/include/airbridge_s3.h` — `ULDBG` upload trace (gated by `UL_TRACE`, default 1; set
  `-DUL_TRACE=0` to compile out). Routed via `airbridge_log` so it reaches CDC+S3 (raw
  `printf` in shared code goes to UART0, never the captured CDC — important gotcha).
- `scripts/repro_sd_contention.sh` — emulator upload repro (`SERIAL`/`DEVICE`/`CONTENTION` env).

## Env / gotchas
- Flash: `ENABLE_CDC` on P1 (USB-visible) → power-cycle → CDC → 1200-baud touch `/dev/ttyACM0`
  → `pio run -e esp32s3 -t upload --upload-port /dev/ttyACM0` → power-cycle. `FORMAT_SD` on P1
  reformats P1+P2 (clears stuck queue). Bucket routing: `EMU_`/`TEST_` device or `TEST.`/
  `EA500.E2E` serial → test bucket, else prod.
- **Do not power-cycle/reconnect in tight loops** — it can throttle the Hologram SIM. The
  device's current bad state is the zombie link, not (only) throttling.

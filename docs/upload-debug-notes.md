# Upload Debugging — State & Next Steps

Working branch: **`gap1-upload-dedup`**. Device: PCB `9C139EF3D3D8` via CoolGear.
Helpers: `scripts/hw_flash.sh` (flash + stage CDC), `scripts/hw_capture.sh <secs> <out>` (cycle + CDC capture).

> PROCESS NOTE: This session twice produced false "findings" by analyzing the WRONG/EMPTY
> capture file. ALWAYS: (1) verify the capture file exists and is non-empty, (2) grep the
> ACTUAL file for `Uploading`/`ULDBG`/`etag=` before drawing conclusions, (3) quote real lines.
> Do not cite a filename you haven't just read.

## RESOLVED — no large-file *upload-code* bug
- Emulator multipart works to BOTH buckets (test via E2E TEST 15; prod via isolation run, 8 MB
  landed intact). SD/MSC contention model completes a 12 MB multipart, no deadlock.
- Branch fixes kept: `Esp32Filesys::readdir` sets `is_dir`; upload task stack 16→32 KB.

## VERIFIED on hardware
1. **Modem is healthy during sessions** (MHEALTH capture `boot_0150`): `CSQ 16–19`, `CEREG 1,5`,
   `CGACT 1,1`, valid IP. `rssi=99` in STATUS is a **stale-display bug** (`g_modemRssi` not
   refreshed in PPP data mode), NOT signal loss.
2. **Small single-PUT uploads work** (capture `/tmp/hw_etag3.log`, RSSI 18, probe-off build):
   a 4063-byte boot log → `presign resp len=1579` (got a response) → single-PUT → `stream done`
   → succeeds. So presign + single-PUT + response-read all work for small bodies.

## NOT YET CAPTURED — the large multipart upload
- In the latest captures the **22 MB file was no longer in the P2 queue** (already harvested on a
  prior boot; FORMAT_SD / reboots cleared it), so the multipart path **did not run** — those
  captures only show `scan found no files but q=N` (the long-standing phantom-counter log) and
  the small-file upload above.
- To exercise multipart again: drop a fresh ≥6 MB `.eaofh` into P1 `flightHistory/` (or use a
  test serial), let it harvest, then capture. The per-part `ULDBG part N etag=[...] resp=...`
  and `uploadId` breadcrumbs are in place (HEAD) and WILL show whether each 5 MB part PUT gets a
  real response, an empty one, or an error.

## Open question (unproven) — why large uploads fail on cellular
Earlier hardware observation (from `ULDBG`, before instrumentation was complete): 22 MB parts
streamed but nothing landed in S3 while the modem was healthy. The MECHANISM is **not yet
pinned** — need a clean multipart capture showing the actual per-part response. Candidates:
- (a) large request body not fully delivered to S3 over cellular → S3 waits, no response;
- (b) connection reset/half-close after the body → read EOF;
- (c) TLS write reports success while bytes are dropped (esp_tls/lwIP/UART).
Root-cause suspects behind these: UART 3 Mbaud + HW flow control RX overrun under sustained
load; lwIP TCP MSS/MTU or checksum config; PPPoS framing.

## Known code bug (fix regardless; emulator-testable)
`airbridge_s3.h` (~line 485): a part PUT with empty ETag is **silently skipped** (not treated as
failure), so the multipart proceeds to `complete` with missing ETags → guaranteed fail. Should
fail+retry the part on an empty/non-200 response.

## NEXT STEPS (emulate-first; hardware only to learn what to emulate)
1. **Re-run a multipart upload on HW**: drop a fresh ≥6 MB `.eaofh`, harvest, `hw_capture.sh`,
   and READ the real per-part `etag`/`resp` + `uploadId` lines. Classify (a)/(b)/(c).
2. **Add deeper part-PUT instrumentation** if needed: log `halStreamFile` bytes-streamed vs
   `Content-Length`, and the `g_hal->network->read` return value in `halHttpReadResponse`
   (>0 / 0=EOF / <0=err) + elapsed time.
3. **A/B the UART link** (921600 vs 3 Mbaud); check `sdkconfig` lwIP `CHECKSUM_*`, UART RX buf / overruns.
4. **Model in `sim_modem.h` / network HAL**: inject the confirmed failure → reproduce locally.
5. **Fix + emulator-validate**: fail+retry on empty part response; plus whatever (a)/(b)/(c) needs.

## Instrumentation in place (HEAD, gap1 branch)
- `airbridge_s3.h`: `ULDBG` trace incl. per-part `etag`/`resp` + `uploadId` (gated `UL_TRACE`,
  default 1). Via `airbridge_log` so it reaches CDC+S3 (raw `printf` → UART0, not CDC).
- `main.cpp`: `modemHealthProbe()` (gated `MODEM_HEALTH_DEBUG`, default 0; +++/ATO disrupts PUTs).
- `scripts/hw_flash.sh`, `scripts/hw_capture.sh`, `scripts/repro_sd_contention.sh`.

## Key files
- `esp32/include/airbridge_s3.h` — `halS3UploadFile` (multipart loop ~409, part response ~481),
  `halStreamFile` (~255), `halS3UploadEaofh`.
- `esp32/include/airbridge_http.h` — `halHttpReadResponse`, `s3ApiGetViaHal`.
- `esp32/src/main.cpp` — `modemTask` pump, PPP, `Esp32Network` (wraps `tls_connect`).
- `esp32/include/sim_modem.h` — SimModem (extend to inject the data-path failure).

## Env / gotchas
- `EA500.E2E`/`EMU_`/`TEST_` → test bucket, else prod. Don't power-cycle in tight loops (SIM throttle).
- Persistent Bash shell got polluted by heredocs earlier — use single-line commands; verify git
  with `git rev-parse --short HEAD`; verify a capture is non-empty + grep the real file before analyzing.

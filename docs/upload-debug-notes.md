# Upload Debugging — State & Next Steps

Working branch: **`gap1-upload-dedup`**. Device: PCB `9C139EF3D3D8` via CoolGear (`scripts/coolgear.py`).

> RETRACTION: an earlier version of this doc claimed a "VERIFIED data corruption on the PPP
> path (empty part responses + corrupted uploadId)." That was WRONG — it was derived from an
> **empty capture file** (the ETag capture produced 0 lines and the flash never ran). There is
> currently **no hardware data** on the per-part PUT response. Treat the per-part behavior as
> UNKNOWN until a clean capture exists.

## RESOLVED — no large-file *upload-code* bug
- **Emulator multipart works** to BOTH buckets. `EMU_E2E` → test bucket (E2E TEST 15, 10 MB).
  Isolation run `SERIAL=EA999.HWDIAG DEVICE=HWDIAG000001 CONTENTION=0` → **prod** bucket, 8 MB
  file **landed intact** (`scripts/repro_sd_contention.sh`). Prod test data was cleaned up.
- **SD/MSC contention model** (`EMU_SD_CONTENTION=1`) completes a 12 MB multipart upload,
  `mscTimeouts=0`, no deadlock → shared lock discipline is sound.
- Real fixes found+kept on the branch: `Esp32Filesys::readdir` now sets `is_dir` (latent bug),
  upload task stack 16→32 KB.
- Conclusion: gap1's upload de-dup is functionally validated; the failure is on the link/data path.

## VERIFIED on hardware (MHEALTH probe capture, `boot_0150`)
- **Modem is healthy during uploads**: `CSQ 16–19`, `CEREG 1,5` (registered), `CGREG 0,5`,
  `CGACT 1,1` (PDP active), valid IP — the whole session.
- **`rssi=99` in STATUS is a stale-display bug** (`g_modemRssi` not refreshed mid-session via
  AUTOCSQ in PPP data mode), NOT signal loss. The "dead radio / zombie link" read was wrong.
- **The upload parts stream** ("stream done", per-MB reads+writes, ~50–95 KB/s) but **no
  multipart/parts ever land in S3** (checked both buckets), while the emulator lands the same
  parts over host internet.

## NOT YET VERIFIED (the open question)
*Why do streamed parts never land in S3 while the modem AT-state is healthy?*
- The per-part PUT response / ETag has **not** been captured. Two attempts produced **empty CDC
  logs (0 lines)**; `scripts/hw_flash.sh` did not exist when invoked (exit 127) so the probe-off
  + ETag build was **never actually flashed** — the device still runs the MHEALTH-probe build.
- Candidate mechanisms to distinguish with a *working* capture: (a) parts don't fully reach S3
  (connection stall/drop → S3 waits, no response); (b) S3 rejects them (bad uploadId/signature
  → error response); (c) data corruption on the UART/PPP path. NONE confirmed yet.

## Known code bug regardless (fix + emulator-test independently)
`airbridge_s3.h` (~line 485): an empty/failed part response (no ETag) is **silently skipped**
instead of fail+retry, so a multipart with any bad part can never complete → upload wedges.

## NEXT STEPS (emulate-first; hardware only to learn what to emulate)
1. **Make the HW flash + CDC capture reliable.** Recreate `scripts/hw_flash.sh` (it was lost),
   run it cleanly (the persistent Bash shell got polluted by multi-line heredocs this session →
   garbled output, cancelled batches, empty captures — use small single-line commands). Confirm
   the probe-off + ETag build is actually flashed and CDC produces output before trusting a run.
2. **Capture the per-part response** (`ULDBG part N etag=[...] resp=...` + the uploadId, hex if
   needed) to classify (a)/(b)/(c). Only then state a mechanism.
3. **Model it in `sim_modem.h`** (drop/corrupt PPP bytes, or stall a part PUT) and reproduce locally.
4. **Fix + emulator-validate**: fail+retry on bad part response; whatever (a)/(b)/(c) shows.

## Instrumentation in place (HEAD, gap1 branch)
- `airbridge_s3.h`: `ULDBG` upload trace + per-part `etag`/`resp` + `uploadId` breadcrumbs
  (gated `UL_TRACE`, default 1). Routed via `airbridge_log` so it reaches CDC+S3 — raw `printf`
  in shared code goes to UART0, never the captured CDC (important gotcha).
- `main.cpp`: `modemHealthProbe()` (gated `MODEM_HEALTH_DEBUG`, default 0 — it does +++/ATO which
  disrupts in-flight PUTs; only enable to read AT-state).
- `scripts/repro_sd_contention.sh` (`SERIAL`/`DEVICE`/`CONTENTION` env). `scripts/hw_flash.sh` is
  MISSING (cancelled write) — recreate it.

## Key files
- `esp32/include/airbridge_modem.h` — `modemAtSync`/`modemRunInitPre/Post`/`modemReconnect`.
- `esp32/src/main.cpp` — `modemTask` pump loop, `pppStale` (~2533), reconnect backoff, PPP events.
- `esp32/include/sim_modem.h` — SimModem (extend to inject PPP-path failure modes).

## Env / gotchas
- Flash: `ENABLE_CDC` on P1 → power-cycle → CDC → 1200-baud touch `/dev/ttyACM0` →
  `pio run -e esp32s3 -t upload --upload-port /dev/ttyACM0` → power-cycle. `FORMAT_SD` on P1
  reformats P1+P2. Bucket routing: `EMU_`/`TEST_` device or `TEST.`/`EA500.E2E` serial → test, else prod.
- Don't power-cycle/reconnect in tight loops (can throttle the Hologram SIM).
- Persistent Bash shell pollution: prefer single-line commands; verify with `git rev-parse --short HEAD`.

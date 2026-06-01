# Upload Debugging — RESOLVED

Working branch: **`gap1-upload-dedup`**. Device: PCB `9C139EF3D3D8` via CoolGear.
Helpers: `scripts/hw_flash.sh`, `scripts/hw_capture.sh <secs> <out>`.

## CONCLUSION (verified, ground-truth 2026-05-31)
**The large-file upload works. There is no upload-code bug and no persistent modem bug.**

Clean hardware capture (`/tmp/hw_mp.log`, fresh 22 MB `.eaofh`, probe-off build, RSSI 18):
- Harvest OK → presign OK → `uploadId` VALID (`svBcu.Ykxoru...`) → `parts=5`.
- **All 5 parts uploaded with valid ETags** (`6a45708a…`, `3922b5c2…`, `463da1d9…`, `460efad3…`,
  `e7dee367…`), ~50 KB/s each.
- `Uploaded … 82 KB/s` → `Cookie updated: flight 1099` → manifest updated.
- **Verified in S3**: `aws s3 ls` shows `EA500.000243_01099_20260531.eaofh` = **22,337,317 bytes**
  (exact size) + updated `manifest.json`. Complete and intact.

So gap1's upload de-dup (firmware running the same shared `airbridge_s3.h` path as the emulator)
is **functionally validated on hardware, including 22 MB multipart over cellular.**

### What the earlier "hangs" actually were
- **Transient cellular/SIM degradation** — induced by excessive power-cycling/reconnecting in
  tight loops (Hologram SIM throttles). When the link is healthy the same code uploads fine.
- **`rssi=99` STATUS stale-display bug** (`g_modemRssi` not refreshed in PPP data mode) made a
  healthy link look dead. It is NOT signal loss (MHEALTH probe showed CSQ 16–19 throughout).

### Theories investigated and DISPROVEN (don't revisit)
- SD-lock starvation / deadlock under MSC contention — emulator contention model completes; HW
  shows reads+writes progressing. NO.
- "Data corruption on PPP / corrupted uploadId" / "empty part responses" — these came from
  analyzing EMPTY or WRONG capture files (process error). The real capture shows valid uploadId +
  valid ETags. NO.
- "Zombie link as a code bug" — modem AT-state is healthy during uploads. The wedge was link
  quality + the stale-display bug, not a missing reconnect path. NO.

## REAL fixes found along the way (KEEP — already on the branch)
1. `Esp32Filesys::readdir` now sets `FsDirEntry::is_dir` (latent bug; `findNextUploadFile` relied
   on it; native impl had it, firmware didn't).
2. Upload task stack 16 KB → 32 KB (the deep `halS3UploadEaofh→halS3UploadFile→s3ApiGetViaHal`
   frame chain overflowed 16 KB → crash on large files). Real, necessary.
3. **Multipart part-PUT retry** (commit `1df4ad3` + `8433a36`): a part PUT that streams fully but
   gets no ETag (lost ack on a flaky link) now retries up to 3× (re-presign + re-seek + re-stream)
   instead of silently skipping → broken `complete`. Aborts cleanly with NVS resume intact if all
   retries fail. Tested: native `test_native_s3` (retry-recovers + all-retries-fail, proven to fail
   with PART_RETRIES=1), and **E2E TEST 24** (OpenSSLNetwork `dropPutResponseOnPart` /
   `EMU_DROP_PUT_RESP` drops the 1st part response → upload still lands; suite 31/31).

## Worthwhile follow-ups (not blockers)
- **`rssi=99` stale-display**: DECIDED to leave as-is (see `feedback_display_direction` memory) —
  no safe way to read RSSI mid-PPP on this single-UART board; users don't need it. Will revisit as
  part of a planned graphical OLED redesign (more graphical, always-show-activity, maybe drop RSSI).
- **Upload-progress watchdog**: considered redundant with existing `pppStale` (90s/120s) which
  already forces reconnect when the modem goes silent; the part-PUT retry covers the app layer.
  Not pursuing unless a real need shows up.
- Network fault-injection harness now exists (`OpenSSLNetwork.dropPutResponseOnPart`) for any
  future flaky-link tests.

## Merge readiness (gap1-upload-dedup)
The upload de-dup + the two real fixes are validated. Before merge to master: turn OFF debug
instrumentation (`-DUL_TRACE=0`, `MODEM_HEALTH_DEBUG` already 0), and decide whether to keep the
diagnostic breadcrumbs (gated off) or strip them.

## Instrumentation / tooling in place (HEAD)
- `airbridge_s3.h`: `ULDBG` upload trace + per-part `etag`/`resp` + `uploadId` (gated `UL_TRACE`,
  default 1; set `-DUL_TRACE=0` to compile out). Via `airbridge_log` → reaches CDC+S3 (raw
  `printf` in shared code → UART0, not the captured CDC — key gotcha).
- `main.cpp`: `modemHealthProbe()` (gated `MODEM_HEALTH_DEBUG`, default 0; +++/ATO disrupts PUTs).
- emulator SD/MSC contention model (`EMU_SD_CONTENTION=1`, `EMU_SD_KBPS`).
- `scripts/hw_flash.sh`, `scripts/hw_capture.sh`, `scripts/repro_sd_contention.sh`.

## Env / gotchas
- `EA500.E2E`/`EMU_`/`TEST_` → test bucket, else prod. Don't power-cycle in tight loops (SIM throttle).
- Verify a capture file is non-empty AND grep the REAL file before drawing conclusions (this
  session twice produced false findings from empty/wrong files). Verify git with
  `git rev-parse --short HEAD` after commits.

# Refactor Plan: Route firmware through the shared `airbridge_*.h` headers

## Problem

The Developer Brief requires that hardware-independent logic live in `esp32/include/airbridge_*.h`
and compile into BOTH the firmware (`main.cpp`) and the emulator/tests, so they run the
same code. For the **upload** and **modem-init** paths this rule is currently violated:
`main.cpp` carries a full raw re-implementation, and the matching HAL functions in the
headers are exercised *only* by the emulator and unit tests. Verified: `main.cpp` makes
**zero** calls to `halS3UploadFile`, `halS3UploadEaofh`, `uploadAllFiles`, `s3ApiGetViaHal`,
`halHttpReadResponse`, `halFetchManifest`, `halUpdateManifest`, or `findNextUploadFile`.

The two copies have already drifted: `airbridge_modem.h::modemRunInit` uses `AT+CREG?`,
while `main.cpp` correctly uses `AT+CEREG?` (the T-Mobile/Hologram fix). The emulator
therefore tests a registration path the firmware no longer uses.

## Duplicated surfaces (firmware copy → header equivalent)

| `main.cpp` (firmware-only copy) | Header equivalent |
|---|---|
| `httpReadResponse` | `halHttpReadResponse` (`airbridge_http.h`) |
| `s3ApiGet` / `s3ApiComplete` | `s3ApiGetViaHal` / `buildApiGetRequest` / `buildApiCompleteRequest` (`airbridge_http.h`) |
| `s3FetchManifest` / `s3UpdateManifest` | `halFetchManifest` / `halUpdateManifest` (`airbridge_s3.h`) |
| `findSplitOffset` | `halFindSplitOffset` (`airbridge_s3.h`) |
| `s3UploadFileEx` (~330 lines) | `halS3UploadFile` + `halS3UploadEaofh` (`airbridge_s3.h`) |
| upload-task file scan + cleanup | `findNextUploadFile` + `markFileUploaded` (`airbridge_s3.h`) |
| upload-task fleet/manifest/delta logic | `halS3UploadEaofh` (`airbridge_s3.h`) |
| multipart NVS resume (raw `nvs_*`) | `loadMultipartSession` / `savePartProgress` / `buildPartsJson` (`airbridge_s3.h`) |
| `modemTask` inline AT-sync + init (~390 lines) | `modemAtSync` / `modemRunInit` (`airbridge_modem.h`) |
| `httpStreamChunk` | `halStreamFile` (`airbridge_s3.h`) |
| `tls_connect` | `Esp32Network::connect` (HAL) |

## Why this is the high-value but high-risk change

This is **flight firmware**. The header paths are well-tested in the emulator, but they
have never run against real `esp_tls` / SIM7600 hardware on the upload path. The plan
de-risks by reconciling first, swapping one subsystem at a time, and keeping the old code
behind a flag until each swap is hardware-verified.

## Step 0 — Reconcile drift FIRST (no behavior change to firmware)

1. ✅ DONE (commit `cb17621`) — `airbridge_modem.h::modemRunInit` now uses `AT+CEREG?`
   (+ `AT+CGREG?` fallback) instead of `AT+CREG?`, matching `main.cpp` and the CLAUDE.md
   "never CREG on T-Mobile/Hologram" rule. `test_native_modem_init` + `test_native_modem`
   pass; the SimModem still answers `AT+CREG?` so the AT-handler tests are unaffected.
2. Diff `halS3UploadFile`/`halS3UploadEaofh` against `s3UploadFileEx` line-by-line and
   note any firmware-only behavior (progress callbacks, `g_sd_mutex` discipline, OLED
   updates, retry counters) that the header lacks. Add those to the header behind the
   HAL so no behavior is lost.

## Step 1 — Verify HAL coverage on the firmware (`Esp32*` impls)

Confirm the firmware's HAL implementations cover everything the headers call:
`g_hal->net` (TLS connect/read/write/close with the same timeouts as `tls_connect`),
`g_hal->fs` (open/read/seek/size with `g_sd_mutex` held), `g_hal->nvs`, `g_hal->clock`.
The streaming throttle in `halStreamFile` must match the firmware's current pacing.

## Step 2 — Swap the modem init path

Replace the inline AT-sync + CFUN/registration/APN/dial block in `modemTask`
(~`:2838–3200`) with calls to `modemAtSync()` + `modemRunInit()`. `modemReconnect()` is
already shared, so this aligns boot and reconnect on one implementation.
**Verify on hardware:** cold boot, soft-reboot (PPP data mode), and carrier-drop reconnect.

## Step 3 — Swap the upload path

Route the upload task through `findNextUploadFile` → `halS3UploadEaofh` / `halS3UploadFile`
→ `markFileUploaded`. This collapses `s3UploadFileEx`, `findSplitOffset`,
`s3ApiGet/Complete/FetchManifest/UpdateManifest`, `httpReadResponse`, `httpStreamChunk`,
and the inline fleet/manifest/delta logic. Delete the firmware-only multipart NVS code in
favor of `loadMultipartSession`/`savePartProgress`/`buildPartsJson` (same `s3up` schema).
**Verify on hardware:** small single-PUT, >5 MB multipart, mid-upload power cut + NVS
resume, fleet manifest skip + delta (e2e TEST 2/3/15/19–23).

## Step 4 — Collapse the incidental duplication

- One `millis()` (drop the duplicate at `main.cpp:83` or the `Esp32Clock` one).
- One NVS access style (use the HAL or the `nvs_get_string` helpers, not three).
- Factor the 78-byte cookie-fetch block (repeated 3× in `uploadTask`) into one helper.
- Factor the `f_getfree` SD-free-space calc (repeated 3×) into one helper.

## Expected result

`main.cpp` drops a further ~700–900 lines and becomes ESP-IDF glue + task wiring only.
The emulator and firmware run identical upload/modem logic, so an emulator-passing change
is meaningful evidence for the firmware. Each step is independently revertable and
hardware-gated.

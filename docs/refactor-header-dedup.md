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

## Coverage findings (2026-05-30 deep analysis)

Goal: maximize how much of the *real firmware runtime* the emulator exercises. The
emulator (`emu/main.cpp`) is a separate orchestration that calls shared **leaf** functions
through the HAL but re-implements the task glue; the firmware runs its own inline copies of
the upload + modem-init paths. Concrete constraints discovered while scoping the swaps:

- **`g_hal` IS fully wired in firmware** (`main.cpp:3692`, `Esp32Display/Clock/Nvs/Filesys/Network`).
  So `halS3UploadFile`/`halS3UploadEaofh`/`findNextUploadFile`/`markFileUploaded` *can* run
  in the firmware unchanged — the swap is mechanically feasible.
- **TLS-tuning risk the emulator cannot catch (Gap 1).** Routing uploads through `halS3*`
  switches the live S3 PUT from the firmware's tuned `tls_connect` to `Esp32Network::connect`.
  The emulator's network HAL is a native socket, so it validates the upload *logic* (multipart
  sequencing, manifest/delta, split-offset, NVS resume) but NOT esp_tls buffer sizes, SNI,
  timeouts, or throughput. → must reconcile `Esp32Network::connect` with `tls_connect`
  (32 KB buffers, timeouts) and **hardware-verify throughput** (target ~167 KB/s) before trusting.
- **Modem init can't be wholesale-replaced by `modemRunInit` (Gap 2).** The firmware's
  `modemTask` does (1) **multi-baud AT sync** (115200 → +++ at 921600/460800/2M/3M, with and
  without HW flow control) and (2) a **baud upgrade to 3 Mbaud + HW flow control that runs
  *between* CCLK time-sync and registration** (`main.cpp:2280–2353`). Both are hardware-specific
  and not meaningfully testable on a single-baud PTY. `modemAtSync`/`modemRunInit` assume a
  fixed 115200 baud. → To share without losing 3Mbaud, split `modemRunInit` into
  `modemRunInitPre` (CFUN reset → ATE0 → CTZU → CCLK→epoch) and `modemRunInitPost`
  (CEREG=1/AUTOCSQ → registration → CSQ → COPS → CGDCONT → dial); firmware calls
  `Pre → (firmware baud upgrade) → Post`; keep `modemRunInit()` as a `Pre+Post` wrapper so the
  emulator and `test_native_modem_init` are unchanged. The firmware must also map the returned
  `ModemInitResult` into its globals (`g_bootEpoch`/`g_bootMs`/`g_modemRssi`/`g_operatorName`)
  and integrate `Post`'s dial with the existing PPP pump loop.
- **Magic-file extraction is low value.** The config *application* (`cliSetWifi`/`cliSetS3`)
  already lives in `airbridge_cli.h` and is unit-tested; only trivial "read 2 lines → apply →
  remove" glue plus hardware actions (`esp_restart`, format, `g_msc_only`) remain in `app_main`.
  Deprioritized relative to Gaps 1 and 2.
- **Validation is serialized on the E2E suite** (~28 min, single emu_nvs/S3-bucket — can't run
  two at once). Each firmware swap needs: native unit tests (fast) → one emulator E2E → a
  hardware sign-off the emulator can't provide. Sequence swaps accordingly.

## Step 0 — Reconcile drift FIRST (no behavior change to firmware)

1. ✅ DONE (commit `cb17621`) — `airbridge_modem.h::modemRunInit` now uses `AT+CEREG?`
   (+ `AT+CGREG?` fallback) instead of `AT+CREG?`, matching `main.cpp` and the CLAUDE.md
   "never CREG on T-Mobile/Hologram" rule. `test_native_modem_init` + `test_native_modem`
   pass; the SimModem still answers `AT+CREG?` so the AT-handler tests are unaffected.
2. Diff `halS3UploadFile`/`halS3UploadEaofh` against `s3UploadFileEx` line-by-line and
   note any firmware-only behavior (progress callbacks, `g_sd_mutex` discipline, OLED
   updates, retry counters) that the header lacks. Add those to the header behind the
   HAL so no behavior is lost.

## Gap 1 — BLOCKERS found (must fix before any upload swap; emulator can't catch these)

Routing the firmware upload through `halS3UploadFile`/`halS3UploadEaofh` is mechanically
feasible (`g_hal` is wired) but two firmware HAL impls are **not** faithful to the live path,
and both failure modes are invisible to the native-socket / no-MSC emulator:

1. **`Esp32Filesys` holds no `g_sd_mutex`** (`main.cpp:243` — plain `fopen/fread`). The live
   `s3UploadFileEx` takes `g_sd_mutex` around every SD read to serialize against the USB MSC
   `tud_msc_read10/write10` callbacks. `halStreamFile` → `g_hal->fs->read` would read the SD
   card with **no mutex** while the host is doing MSC I/O → corruption / crashes on hardware.
   Fix: give `Esp32Filesys` the same take-read-give mutex discipline (per-chunk, releasing
   during the slow TLS write so MSC isn't starved) before routing uploads through it.
2. **`Esp32Network::connect` ≠ `tls_connect`** (`main.cpp:276` vs `:1210`). It omits:
   - `g_tlsActive = true` — the modem watchdog uses `g_tlsActive ? 120000 : 90000` for the
     pppStale threshold; without it, a long upload can trip a false reconnect mid-transfer.
   - socket `SO_RCVTIMEO`/`SO_SNDTIMEO = 30s` — without them a stalled cellular socket can
     hang well past the handshake `timeout_ms`.
   Fix: make `Esp32Network::connect`/`destroy` a faithful wrapper of `tls_connect`/`tls_destroy`
   (set/clear `g_tlsActive`, set socket timeouts, keep the detailed error logging).

Only after (1) and (2) does the upload swap preserve behavior. Then **hardware-verify
throughput** (target ~167 KB/s, 10 MB multipart) — esp_tls buffer/SNI/throughput is the one
thing the emulator fundamentally cannot validate. Recommended: do (1)+(2) as their own commit
(no behavior change — the live path keeps working), hardware-check, THEN swap the upload path.

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

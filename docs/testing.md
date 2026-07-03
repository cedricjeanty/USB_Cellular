# Testing

## ESP32-S3 (active)

The active variant has two test layers, both running the shared `airbridge_*.h` logic:

- **Native Unity unit tests** — `esp32/test/test_native_*/` (22 suites: proto, dsu,
  harvest, modem, modem_init, http, s3, ppp, nvs, cli, display, runtime, triggers,
  utils, sd_block, sd_format, commands, command_fetch, compress, fatfsfs, log,
  net_util). Run on the host via PlatformIO native env, no hardware.
  ```bash
  cd esp32 && pio test -e native
  ```
- **End-to-end suite** — `scripts/e2e_unified.sh` runs scenario tests against the SDL2
  emulator or real hardware:
  ```bash
  scripts/e2e_unified.sh --target emulator   # SimModem PTY + FakeSD, no hardware
  scripts/e2e_unified.sh --target device     # CoolGear-power-cycled hardware
  ```
  Several tests are **consolidated**: rather than booting the emulator per case, they run a
  natural sequence on one session and assert at each step — which also tests emergent,
  cross-operation behavior (manifest HWM monotonic, cookie chains, state survives reboot):
  - **TEST 2 — happy-path lifecycle**: upload → multi-file → system-file-skip → cookie →
    reboot/persist → PPP drop+reconnect → upload-order, in one boot.
  - **TEST 19 — manifest lifecycle**: full-upload → skip-covered-file → boot cookie sync.
  - **TEST 3 — power-cut resume ×2**: single-PUT cut+resume, then multipart cut + NVS resume.

  Waits are **state-triggered** off log markers (`wait_for_log`, `wait_for_harvest`,
  `wait_for_upload_complete`, `wait_for_manifest`) rather than fixed sleeps, so each step and
  each power-cut lands at the correct moment. Isolation-critical tests (other power-cut /
  regression cases) stay standalone.

### Running the consolidated suite on hardware (`--target device`)

The `device` target now runs the SAME consolidated tests as the emulator (TEST 2, the
multipart-NVS half of TEST 3, TEST 13, TEST 19, TEST 21). Only TEST 24 (dropped-response
fault injection) stays emulator-only. Three pieces make this work:

1. **Persistent CDC + serial tap.** The whole device run uses CDC+MSC so a serial log is
   available throughout (state-triggered waits work on hardware). Flash the
   **`esp32s3-e2e`** env (`-DALLOW_CDC_PERSIST`), then the harness drops a `CDC_PERSIST`
   magic file on P1 — unlike one-shot `ENABLE_CDC`, it is re-read every boot and never
   deleted, so every boot is CDC+MSC until the file is removed (teardown restores
   production MSC-only). CDC enumerates at boot while the MSC LUN stays not-ready for 90s,
   so the **90s presentation timing is preserved**. A background reader (`serial_tap_start`,
   modeled on `scripts/hw_capture.sh`) mirrors `/dev/ttyACM*` into `/tmp/dev_e2e.log`
   (`$E2E_LOG`), reconnecting across power cycles.
2. **Per-target marker table.** Emulator progress strings (`Init complete`, `[Harvest]
   Done:`, …) are `printf` in `emu/main.cpp` and never appear on serial; the firmware emits
   different strings, and only `airbridge_log`/`cdc_printf`/`ULDBG` reach CDC (NOT
   `ESP_LOGI`). A `marker <event>` helper resolves each logical event to the right regex
   per target.
3. **Cookie-aware host DSU** — `scripts/host_dsu.py` (port of `esp32/include/sim_dsu.h`).
   It mounts the ESP32's USB volume (retrying until the 90s MSC delay passes),
   CRC-validates `dsuCookie.easdf`, and emits only flights past the cookie — exactly like a
   real aircraft DSU. Backs `write_dsu_file` on device. It synthesizes records at the
   requested flight numbers (the real fixtures only span flights 1071-1077); `--mode slice`
   does a true byte-slice of a fixture for highest fidelity. Other modes: `--plant-cookie`,
   `--read-cookie`, `--partial` (power-loss test), `--ignore-cookie` (force-feed a covered
   flight to exercise the firmware's manifest-skip).

Prereq: device flashed with `pio run -e esp32s3-e2e -t upload`; CoolGear hub on
`/dev/ttyUSB0`; `sudo` mount rights. Two device gaps are intentional: the active
PPP-drop+reconnect sub-assertion (TEST 2 step F — no host-side cellular-only drop exists),
and the production MSC-only D+-low invisibility (covered by an opt-in final check,
`E2E_MSCONLY_CHECK=1`, since the suite otherwise runs CDC+MSC throughout). The device run
takes ~70-85 min (90s boot + modem connect per cycle, plus TEST 18's 10-min soak).

The sections below cover the **legacy Raspberry Pi** pytest suite.

## Unit tests (no hardware required)

```bash
pytest tests/test_unit.py
```

## Hardware tests (Pi)

```bash
# All tests against the Pi
pytest --pi-host cedric@pizerologs.local

# + disruptive USB gadget tests
pytest --pi-host cedric@pizerologs.local --disruptive

# + WiFi upload / e2e tests
pytest --pi-host cedric@pizerologs.local --disruptive --cellular

# + USB cable write tests
pytest --pi-host cedric@pizerologs.local --usb-device /dev/sdb
```

## Test files and marks

| File | Tests | Flags required |
|------|-------|----------------|
| `tests/test_unit.py` | 66 | none — runs anywhere |
| `tests/test_hardware.py` | 18 | `--pi-host` / `--disruptive` |
| `tests/test_e2e.py` | 4 | `--pi-host --disruptive --cellular` |
| `tests/test_usb_write.py` | 3 | `--pi-host --usb-device /dev/sdX` |

## Mark reference

| Mark | Meaning | Enable with |
|------|---------|-------------|
| `hardware` | SSH to Pi required | `--pi-host USER@HOST` |
| `cellular` | WiFi + data required | `--cellular` |
| `disruptive` | Briefly unloads USB gadget | `--disruptive` |
| `usb_cable` | Pi connected via USB cable | `--usb-device /dev/sdX` |
| `slow` | Takes > 30s (informational) | never skipped |

## conftest fixtures

| Fixture | Scope | Description |
|---------|-------|-------------|
| `ssh` | session | SSHRunner (run/check/python); auto-skips if Pi unreachable |
| `pi_config` | session | config.yaml parsed from Pi; depends on ssh |
| `usb_device` | session | Block device path from --usb-device flag |

## FTP test server

- sftpcloud.io gives a free 60-min FTP server at eu-central-1.sftpcloud.io port 21
- Script: `scripts/get_ftp_creds.py` — grabs credentials via headless Chrome (Selenium)
- Auto-updates config.yaml, deploys to Pi, restarts airbridge service
- Run before any upload test: `python3 scripts/get_ftp_creds.py`
- Options: `--dry-run`, `--no-deploy`, `--show-browser`, `--pi-host USER@HOST`

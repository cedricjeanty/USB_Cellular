# Testing

## ESP32-S3 (active)

The active variant has two test layers, both running the shared `airbridge_*.h` logic:

- **Native Unity unit tests** — `esp32/test/test_native_*/` (16 suites: proto, dsu,
  harvest, modem, modem_init, http, s3, ppp, nvs, cli, display, runtime, triggers,
  utils, sd_block, sd_format). Run on the host via PlatformIO native env, no hardware.
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

  Waits are **state-triggered** off emulator log markers (`wait_for_log`, `wait_for_harvest`,
  `wait_for_upload_complete`, `wait_for_manifest`) rather than fixed sleeps, so each step and
  each power-cut lands at the correct moment. Isolation-critical tests (other power-cut /
  regression cases) stay standalone.

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

# Testing

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

# Test Suite

Tests for the USB Airbridge service, using pytest.

## Files

| File                 | Description                                     |
|----------------------|-------------------------------------------------|
| `test_unit.py`       | Pure unit tests — no hardware required          |
| `test_hardware.py`   | On-Pi hardware checks (I2C, WiFi, USB gadget)   |
| `test_e2e.py`        | End-to-end harvest + upload tests               |
| `conftest.py`        | Shared fixtures, marks, SSH helper              |

## Quick Usage

```bash
# Unit tests only — run anywhere, no Pi needed
pytest tests/test_unit.py

# All hardware tests (SSH to Pi required)
pytest --pi-host cedric@pizerologs.local

# + tests that briefly unload the USB gadget
pytest --pi-host cedric@pizerologs.local --disruptive

# + WiFi upload / e2e tests
pytest --pi-host cedric@pizerologs.local --disruptive --cellular
```

## Marks

| Mark        | Description                               | Enable with              |
|-------------|-------------------------------------------|--------------------------|
| `hardware`  | SSH to Pi required                        | `--pi-host USER@HOST`    |
| `cellular`  | Requires WiFi network connection          | `--cellular`             |
| `disruptive`| Briefly unloads USB gadget                | `--disruptive`           |
| `usb_cable` | Pi connected via USB cable to this host   | `--usb-device /dev/sdX`  |
| `slow`      | Takes > 30 s (informational)              | never skipped            |

## Legacy Scripts

`usb_test.py` and `integration_test.py` are older standalone scripts that predate
the pytest suite. They can still be run directly on the Pi for quick manual checks,
but the pytest suite is the primary test mechanism.

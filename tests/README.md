# Test Scripts

This directory contains test scripts for validating the USB Cellular Airbridge functionality.

## Files

- `e2e_test.py` - End-to-end integration tests
- `ftp_test.py` - FTP upload functionality tests  
- `sim7000_test.py` - SIM7000G cellular modem tests
- `usb_test.py` - USB gadget and mass storage tests

## Usage

Run individual tests directly:
```bash
python3 usb_test.py
python3 sim7000_test.py
python3 ftp_test.py
python3 e2e_test.py
```

Tests should be run on the target Raspberry Pi hardware with the SIM7000G HAT attached.
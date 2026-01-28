# Test Scripts

This directory contains test scripts for validating the USB Cellular Airbridge functionality.

## Files

### Core Tests (Run on Raspberry Pi)
- `e2e_test.py` - End-to-end integration tests
- `ftp_test.py` - FTP upload functionality tests  
- `sim7000_test.py` - SIM7000G cellular modem tests
- `usb_test.py` - USB gadget and mass storage tests

### Integration Tests (Run on Host Machine)
- `integration_test.py` - **NEW** Full integration test with connected USB Air Bridge device
- `ssh_device_test.py` - **NEW** SSH connectivity and remote device testing
- `test_config.yaml` - **NEW** Configuration for integration testing

## Usage

### Traditional Tests (Run on Pi)
Run individual tests directly on the Raspberry Pi:
```bash
python3 usb_test.py
python3 sim7000_test.py
python3 ftp_test.py
python3 e2e_test.py
```

### Integration Tests (Run on Host)
Run integration tests from a host machine with the Air Bridge device connected:

```bash
# Full integration test - assumes USB Air Bridge connected via USB and SSH
python3 integration_test.py

# SSH-only device testing 
python3 ssh_device_test.py

# Show help
python3 integration_test.py --help
python3 ssh_device_test.py --help
```

## Test Assumptions

### Traditional Tests
- Tests should be run on the target Raspberry Pi hardware with the SIM7000G HAT attached
- Device has direct access to hardware interfaces

### Integration Tests  
- **USB Air Bridge device is physically connected via USB** to the host machine
- **Device is accessible via SSH** at `pizerologs.local`
- Host machine has sudo privileges for USB device mounting
- Both host and device have Python 3 with required packages

## Configuration

Integration tests use `test_config.yaml` for configuration. Key settings:

- **Device connection**: SSH host, user, and USB device detection
- **Test parameters**: File sizes, timeouts, test scenarios  
- **Expected device config**: Service names, file paths, hardware interfaces
- **Troubleshooting**: Common issues and solutions

## Test Scenarios

### Basic Connectivity (SSH Device Test)
1. SSH connectivity verification
2. Device information gathering
3. Service status checking
4. Hardware interface validation
5. Python environment testing

### Full Integration Test
1. **USB Device Detection**: Auto-detect connected Air Bridge USB device
2. **File Transfer**: Create test files on USB device 
3. **File Harvesting**: Wait for Air Bridge to detect and harvest files
4. **Upload Verification**: Confirm files uploaded via cellular
5. **Log Analysis**: Check device logs for issues
6. **Cleanup**: Reset device state

## Troubleshooting

See `test_config.yaml` for common issues and solutions, including:
- SSH connection failures
- USB device detection problems  
- Service startup issues
- File harvesting timeouts

## Requirements

### Host Machine
- Python 3 with `subprocess`, `time`, `uuid` modules
- SSH client configured for passwordless access to device
- Sudo access for USB device mounting

### Air Bridge Device  
- SSH server running and accessible
- Airbridge service installed and configured
- Python 3 with airbridge modules and dependencies
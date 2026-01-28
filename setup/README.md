# Setup and Installation

This directory contains scripts and configuration files for setting up the USB Cellular Airbridge on a Raspberry Pi Zero.

## Files

- `setup_otg.sh` - Configures USB OTG mode and mass storage gadget
- `setup_readonly.sh` - Sets up read-only filesystem for reliability
- `maintenance.sh` - System maintenance and monitoring utilities
- `airbridge.service` - systemd service file for main application
- `airbridge-web.service` - systemd service file for web status interface

## Usage

1. Deploy code to Pi: `scp ../src/*.py config.yaml *.sh cedric@pizerologs.local:~`
2. Run OTG setup: `ssh cedric@pizerologs.local "sudo bash setup_otg.sh"`
3. Install services: Copy .service files to `/etc/systemd/system/`
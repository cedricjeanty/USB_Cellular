# Source Code

This directory contains the main application source code.

## Structure

- `airbridge/` - Main USB Cellular Airbridge application
  - `main.py` - Primary service with state machine logic
  - `modem_handler.py` - SIM7000G cellular modem interface

- `web/` - Web status interface
  - `web_status.py` - Flask-based status web server
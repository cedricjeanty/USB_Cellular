# Source Code

This directory contains the main application source code.

## Structure

- `airbridge/` - Main USB Airbridge application
  - `main.py` - Primary service with state machine logic
  - `wifi_manager.py` - WiFi status, FTP/HTTP upload over wlan0
  - `captive_portal.py` - NM hotspot AP with captive portal for WiFi provisioning
  - `display_handler.py` - SSD1306 OLED status display

- `web/` - Web status interface
  - `web_status.py` - Flask-based status web server
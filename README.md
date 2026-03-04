# USB Airbridge

A Python service that bridges a Legacy Host (2004-era Linux) to a cloud server via Raspberry Pi Zero 2 W and WiFi.

## Project Structure

```
├── src/                    # Main application source code
│   ├── airbridge/         # Core airbridge service
│   └── web/               # Web status interface
├── setup/                 # Installation and configuration
├── tests/                 # Test scripts
├── config.yaml            # Main configuration file
└── CLAUDE.md              # Developer documentation
```

## Quick Start

1. **Setup**: Deploy using scripts in `setup/`
2. **Configure**: Edit `config.yaml` for your environment
3. **Run**: Use systemd services or run directly from `src/airbridge/`
4. **Test**: Validate functionality with scripts in `tests/`

## How It Works

The system operates in a duty cycle:

1. **Data Collection**: Pi appears as USB mass storage to Legacy Host
2. **Log Harvesting**: Pi takes ownership, mounts filesystem, collects new files
3. **WiFi Upload**: Uploads files via wlan0 using FTP or HTTP

If WiFi credentials are not yet provisioned, the Pi creates an open "AirBridge" AP. Connect your phone and a captive portal lets you enter WiFi credentials.

See `CLAUDE.md` for detailed technical documentation.
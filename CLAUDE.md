# Developer Brief: USB WiFi Airbridge

Two hardware variants exist: a Raspberry Pi Zero 2 W (legacy, being deprecated) and an
ESP32-S3 (active development on `esp32-s3` branch). Both present a USB mass storage
device to a legacy host, harvest files when idle, and upload via WiFi.

## ESP32-S3 Variant (Active)

**Hardware:** ESP32-S3-DevKitC-1 (4 MB flash, 2 MB PSRAM)

**USB:** TinyUSB mass storage (ARDUINO_USB_MODE=0) + CDC serial

**Storage:** SD card via SPI (CS=10, MOSI=11, MISO=12, SCK=13)

**Display:** SSD1306 128×64 OLED at I2C GPIO 7 (SCL) / 8 (SDA), addr 0x3C

**Upload:** S3 via pre-signed URLs from a Lambda backend (no AWS creds on device)

**Source:** `esp32/src/main.cpp` (PlatformIO)

**Two firmware branches:**
- `esp32-s3` — Arduino framework (90 KB/s upload, legacy)
- `esp32-idf` — ESP-IDF native (279 KB/s upload, active development)
  - 32KB TCP send/receive buffers (vs Arduino's fixed 5760)
  - Custom sdkconfig for lwIP/mbedTLS tuning
  - Uses esp_tls + cert bundle for HTTPS
  - Local esp_tinyusb component for custom MSC callbacks

### S3 Upload Architecture

```
ESP32 → GET /presign (API Gateway + Lambda) → pre-signed S3 PUT URL
ESP32 → PUT file data → S3 bucket (direct)
```

- Lambda: `airbridge-presign` in us-west-2, source in `lambda/presign.py`
- API Gateway requires `x-api-key` header (fleet key, rate-limited)
- Small files (<5 MB): single PUT. Large files: S3 multipart with NVS resume.
- Files land in S3 as `<device_mac>/<filename>`
- Device ID auto-generated from WiFi MAC address

### ESP32 NVS Namespaces

| Namespace | Keys | Purpose |
|-----------|------|---------|
| `s3` | `api_host`, `api_key`, `device_id` | Upload credentials |
| `s3up` | `name`, `uid`, `key`, `parts`, `part`, `etagN` | Multipart resume state |
| `wifi` | `ssid0`..`ssid4`, `pass0`..`pass4`, `count` | Saved WiFi networks |

### ESP32 Serial CLI

| Command | Description |
|---------|-------------|
| `SETWIFI <ssid> <pass>` | Save a WiFi network |
| `SETS3 <api_host> <api_key>` | Set S3 upload backend |
| `STATUS` | Show WiFi, upload stats, S3 config |
| `UPLOAD` | Trigger upload task manually |
| `FORMAT` | Format SD card (destroys all data) |
| `REBOOT` | Reboot device |

### Flashing (device looks like a USB drive when running)

1. **1200-baud touch:** open CDC port at 1200 baud and close → enters bootloader
   - Arduino branch: enters JTAG bootloader, flash on new ttyACM port
   - ESP-IDF branch: enters ROM USB bootloader (303a:0009), flash on ttyACM0
   - **ESP-IDF: must power cycle after flash** (ROM bootloader doesn't auto-reset)
2. **Manual download mode:** hold BOOT, press RESET → flash normally

### Build

```bash
cd esp32 && ~/.local/bin/pio run          # compile only
cd esp32 && ~/.local/bin/pio run -t upload # compile + flash
```

### ESP-IDF Branch Notes

- **Power cycle required** after flashing (not soft REBOOT) — stale SPI state
- SD card must be formatted by ESP-IDF FATFS on first boot (`format_if_mount_failed=true`, then set to `false`)
- `sdkconfig.defaults` in esp32/ controls lwIP/mbedTLS tuning
- Delete `sdkconfig.esp32s3` to force sdkconfig.defaults to be re-applied
- Local `components/esp_tinyusb/` overrides default MSC storage (custom read/write tracking)

---

## Raspberry Pi Variant (Legacy — being deprecated)

**Environment:** Raspberry Pi Zero 2 W running Raspberry Pi OS (Lite)

**Access:** `cedric@pizerologs.local`

**Hardware Interface:** USB-OTG port in peripheral mode — emulates a Mass Storage Device (USB Gadget)

**Connectivity:** WiFi via wlan0 (built-in); captive portal AP for credential provisioning

**Display:** SSD1306 128×64 OLED at I2C-1 address 0x3C

## 0. Pi Zero Setup (Run Once)

### Deploy code to Pi:
```bash
scp src/airbridge/*.py config.yaml cedric@pizerologs.local:~
# For persistent storage across overlayroot reboots:
ssh cedric@pizerologs.local "sudo mount -o remount,rw /media/root-ro"
scp src/airbridge/*.py config.yaml "cedric@pizerologs.local:/media/root-ro/home/cedric/"
```

### Enable USB peripheral mode:
In `/boot/firmware/config.txt`, ensure the `[all]` section contains:
```
dtoverlay=dwc2,dr_mode=peripheral
```

**Important:** The dwc2 overlay MUST be in the `[all]` section, not under `[cm4]` or `[cm5]`.

### Manual USB gadget commands:
```bash
# Load gadget (Pi appears as USB drive)
sudo modprobe g_mass_storage file=/dev/mmcblk0p3 stall=0 removable=1

# Check UDC state
cat /sys/class/udc/*/state

# Unload gadget (before mounting locally)
sudo modprobe -r g_mass_storage

# Mount locally to read files
sudo mount /dev/mmcblk0p3 /mnt/usb_logs
```

## 1. Context & Objective

Python service that bridges a **Legacy Host (2004-era Linux)** to a cloud server. The Legacy Host treats the Pi as a standard USB thumb drive. The Pi intermittently harvests files and uploads them via WiFi (wlan0) to a remote FTP or HTTP server.

## 2. Technical Stack

* **WiFi:** `nmcli` (NetworkManager) for connection management; `/proc/net/wireless` for RSSI
* **Captive Portal:** NM hotspot AP (`nmcli con add type wifi mode ap`) + Python HTTPServer
* **System Operations:** `subprocess` for `modprobe`, `mount`, `umount`, `nmcli`
* **File Management:** `os`, `shutil`, `zipfile` for log handling
* **Uploads:** `ftplib` / `requests` directly over wlan0 (no AT command overhead)
* **Logging:** Robust local logging via Python `logging` → systemd journal

## 3. Duty Cycle

### Phase 1: Data Collection (Host Ownership)

1. Load `g_mass_storage` gadget pointing to `/dev/mmcblk0p3`.
2. Monitor UDC state: `cat /sys/class/udc/*/state`.
3. If `state == 'configured'`, watch disk write activity.
4. Proceed to Phase 2 only if no writes have occurred for N seconds (quiet window).

### Phase 2: Log Harvesting (Pi Ownership)

1. Unload USB Gadget: `sudo modprobe -r g_mass_storage`.
2. Mount partition locally: `sudo mount /dev/mmcblk0p3 /mnt/usb_logs`.
3. Compress new files into `.zip` archives.
4. Move archives to `/home/cedric/outbox/` queue.
5. Unmount and reload gadget.

### Phase 3: WiFi Upload

1. Check WiFi connectivity (`wifi_manager.is_connected()`).
2. If no WiFi: start captive portal AP so user can provide credentials via phone browser.
3. If WiFi available: upload queued files via FTP or HTTP.
4. Delete successfully uploaded files from outbox.

## 4. Captive Portal

When WiFi is unavailable, the Pi creates an open AP named "AirBridge":

- NM hotspot at `192.168.4.1/24` (`nmcli con add type wifi mode ap ipv4.method shared`)
- NM's internal dnsmasq redirects all DNS to AP IP via `/etc/NetworkManager/dnsmasq-shared.d/`
- Python HTTPServer on port 80 serves a credential entry form
- iOS/Android detect the DNS redirect and show "Sign in to network" prompt
- On form submit: portal stops, `add_network()` provisions the new WiFi network

**Critical ordering:** call `portal.stop()` BEFORE `add_network()` so wlan0 is back in
client mode when attempting to join the new network.

## 5. Files

### ESP32-S3 (both branches)
* `esp32/src/main.cpp` — Single-file firmware: USB MSC, harvest, S3 upload, WiFi, OLED, CLI
* `esp32/platformio.ini` — PlatformIO build config (Arduino on esp32-s3, ESP-IDF on esp32-idf)
* `esp32/sdkconfig.defaults` — ESP-IDF branch only: lwIP/mbedTLS tuning (32KB TCP buffers)
* `esp32/components/esp_tinyusb/` — ESP-IDF branch only: local TinyUSB component with custom MSC callbacks
* `lambda/presign.py` — Lambda function for S3 pre-signed URL generation

### Raspberry Pi (legacy)
* `src/airbridge/main.py` — Primary state machine (harvest loop + upload worker thread)
* `src/airbridge/wifi_manager.py` — WiFi status, FTP/HTTP upload
* `src/airbridge/captive_portal.py` — NM hotspot AP + captive portal HTTP server
* `src/airbridge/display_handler.py` — SSD1306 OLED display
* `config.yaml` — FTP server settings, poll interval, `wifi_ap` ssid/channel

## 6. Configuration (`config.yaml`)

```yaml
virtual_disk_path: /dev/mmcblk0p3
mount_point: /mnt/usb_logs
outbox_dir: /home/cedric/outbox
quiet_window_seconds: 30
poll_interval: 5

wifi_ap:
  ssid: AirBridge
  channel: 6

upload_method: ftp   # or http

ftp:
  server: your-ftp-server.example.com
  port: 21
  username: user
  password: pass
  remote_path: /uploads
```

## 7. Constraints

* **Single-Host Violation:** NEVER mount the partition while `g_mass_storage` is active — immediate filesystem corruption.
* **Power Resilience:** Use `os.sync()` after moving files; assume power could be cut at any moment.
* **Overlayroot filesystem:** `/home/cedric/` is on a tmpfs overlay that resets on reboot. For persistent changes deploy to `/media/root-ro/home/cedric/` (after `sudo mount -o remount,rw /media/root-ro`).
* **nmcli con up as Popen:** `nmcli con up airbridge-ap` must be fire-and-forget (Popen) because the SSH session drops when the Pi switches from client to AP mode.

## 8. Testing

```bash
# Unit tests — no hardware required
pytest tests/test_unit.py

# All tests against the Pi
pytest --pi-host cedric@pizerologs.local

# + disruptive USB gadget tests
pytest --pi-host cedric@pizerologs.local --disruptive

# + WiFi upload / e2e tests
pytest --pi-host cedric@pizerologs.local --disruptive --cellular
```

configure the raspberry pi OTG port as a USB drive. wait for the file system to stop growing for 30s, and then unmount, and display the files that were added.

take a directory of files, and upload them through WiFi to an FTP server.

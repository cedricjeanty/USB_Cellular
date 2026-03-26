# Raspberry Pi Variant (Legacy — being deprecated)

## Hardware

- **Board:** Raspberry Pi Zero 2 W
- **Access:** `cedric@pizerologs.local`
- **USB:** OTG port in peripheral mode (`dr_mode=peripheral` in `/boot/firmware/config.txt`)
- **WiFi:** Built-in wlan0, captive portal AP for credential provisioning
- **Display:** SSD1306 128×64 OLED at I2C-1 address 0x3C

## Setup (Run Once)

### Enable USB peripheral mode
In `/boot/firmware/config.txt`, ensure the `[all]` section contains:
```
dtoverlay=dwc2,dr_mode=peripheral
```
**Important:** The dwc2 overlay MUST be in the `[all]` section, not under `[cm4]` or `[cm5]`.

### USB gadget commands
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

## Technical Stack

* **WiFi:** `nmcli` (NetworkManager); `/proc/net/wireless` for RSSI
* **Captive Portal:** NM hotspot AP + Python HTTPServer
* **System Operations:** `subprocess` for `modprobe`, `mount`, `umount`, `nmcli`
* **File Management:** `os`, `shutil`, `zipfile` for log handling
* **Uploads:** `ftplib` / `requests` directly over wlan0
* **Logging:** Python `logging` → systemd journal

## Duty Cycle

### Phase 1: Data Collection (Host Ownership)
1. Load `g_mass_storage` gadget pointing to `/dev/mmcblk0p3`
2. Monitor UDC state: `cat /sys/class/udc/*/state`
3. If `state == 'configured'`, watch disk write activity
4. Proceed to Phase 2 only if no writes for N seconds (quiet window)

### Phase 2: Log Harvesting (Pi Ownership)
1. Unload USB Gadget: `sudo modprobe -r g_mass_storage`
2. Mount partition locally: `sudo mount /dev/mmcblk0p3 /mnt/usb_logs`
3. Compress new files into `.zip` archives
4. Move archives to `/home/cedric/outbox/` queue
5. Unmount and reload gadget

### Phase 3: WiFi Upload
1. Check WiFi connectivity (`wifi_manager.is_connected()`)
2. If no WiFi: start captive portal AP for credential provisioning
3. If WiFi available: upload queued files via FTP or HTTP
4. Delete successfully uploaded files from outbox

## Captive Portal

- NM hotspot at `192.168.4.1/24` (`nmcli con add type wifi mode ap ipv4.method shared`)
- NM's internal dnsmasq redirects all DNS to AP IP via `/etc/NetworkManager/dnsmasq-shared.d/`
- Python HTTPServer on port 80 serves a credential entry form
- iOS/Android detect the DNS redirect and show "Sign in to network" prompt
- **Critical ordering:** call `portal.stop()` BEFORE `add_network()` so wlan0 is back in client mode

## Configuration (`config.yaml`)

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

## Constraints

* **Single-Host Violation:** NEVER mount the partition while `g_mass_storage` is active
* **Power Resilience:** Use `os.sync()` after moving files; assume power could be cut at any moment
* **Overlayroot filesystem:** `/home/cedric/` resets on reboot. Deploy to `/media/root-ro/home/cedric/`
* **nmcli con up as Popen:** fire-and-forget because SSH session drops when Pi switches to AP mode

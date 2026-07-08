# Build & Flash

## Build

```bash
cd esp32 && ~/.local/bin/pio run          # compile only
cd esp32 && ~/.local/bin/pio run -t upload # compile + flash
```

## Flashing (device looks like a USB drive when running)

### Arduino firmware (`esp32-s3` branch)

1. **1200-baud touch:** open CDC port at 1200 baud and close
   - `python3 -c "import serial,time; s=serial.Serial('/dev/ttyACM0',1200); time.sleep(0.3); s.close()"`
   - Device enters JTAG bootloader — new `/dev/ttyACM1` appears
   - `~/.local/bin/pio run -t upload --upload-port /dev/ttyACM1`
   - Device boots directly into new firmware after flash
   - Do NOT do a second 1200-baud reset after flashing

### ESP-IDF firmware (`esp32-idf` branch)

1. **1200-baud touch:** same as above, but enters ROM USB bootloader (303a:0009)
   - `~/.local/bin/pio run -t upload --upload-port /dev/ttyACM0`
   - **Must power cycle after flash** (ROM bootloader doesn't auto-reset via USB)
   - Must also power cycle (not soft REBOOT) between sessions — soft reset leaves stale SPI state

### Manual download mode (both branches)

Hold BOOT button, press RESET → appears as /dev/ttyACM0, flash normally.

### Button-free flash / recovery (`scripts/jtag_flash.py`)

Normal button-free flashing is the 1200-baud touch above — but that needs the firmware
to boot far enough to present CDC. If a bad build hangs in early boot it presents
nothing usable, and historically the only recovery was the (often inaccessible) BOOT
button. `scripts/jtag_flash.py` recovers WITHOUT the button: at power-on the chip briefly
exposes its USB-Serial-JTAG (`303a:1001`) before the app switches USB to TinyUSB; a *cold*
esptool misses that <1s window, so the script keeps a **warm (pre-imported) esptool** busy-
waiting and fires `default-reset` the instant the JTAG enumerates — landing in the window and
dropping the chip into stable ROM download mode. It then `erase`s (clears any stale OTA
selector — the cause of one boot-loop) and writes the full image. Uses the CoolGear hub for
power if present, else `--manual` prompts. `--app-bin latest.bin` flashes a prod app over the
env's bootloader/partitions. **Note:** `pio run -t upload` writes only the app slot and can
leave a stale OTA selector → boot a wrong/garbage slot; prefer this script (or `pio run -t
erase` first) when the OTA partition state is uncertain.

## ESP-IDF sdkconfig

- `sdkconfig.defaults` in `esp32/` controls lwIP/mbedTLS tuning
- Delete `sdkconfig.esp32s3` to force sdkconfig.defaults to be re-applied on next build
- Key settings:
  - `CONFIG_LWIP_TCP_SND_BUF_DEFAULT=32768` (upload throughput)
  - `CONFIG_LWIP_TCP_WND_DEFAULT=32768`
  - `CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384` (S3 cert chain needs ≥16KB)
  - `CONFIG_ESP_TLS_INSECURE=y` + `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y`
  - `CONFIG_FATFS_SECTOR_512=y` (not 4096)
  - `CONFIG_SPIRAM=n` (PSRAM init fails on this board variant)

## SD Card Format

- Arduino SdFat FORMAT creates MBR with partition at sector 8192
- ESP-IDF FATFS creates MBR with partition at sector 63
- The two formats are NOT interchangeable — switching branches requires reformatting
- ESP-IDF: set `format_if_mount_failed=true` for first boot, then `false` for production
- After FATFS format, run `sudo partprobe /dev/sda` on the host to refresh Linux's partition table

## Deploying to Pi (legacy)

```bash
scp src/airbridge/*.py config.yaml cedric@pizerologs.local:~
# For persistent storage across overlayroot reboots:
ssh cedric@pizerologs.local "sudo mount -o remount,rw /media/root-ro"
scp src/airbridge/*.py config.yaml "cedric@pizerologs.local:/media/root-ro/home/cedric/"
```

- Restart service: `ssh cedric@pizerologs.local "sudo systemctl restart airbridge"`
- Check logs: `ssh cedric@pizerologs.local "sudo journalctl -u airbridge -f"`

# ESP32-S3 Deployer Memory

## Project: /home/cedric/USBCellular/esp32/
- Board: esp32-s3-devkitc-1 (4 MB flash, 2 MB PSRAM embedded)
- Framework: Arduino via PlatformIO
- PlatformIO env name: esp32s3
- Build command: `~/.local/bin/pio run -e esp32s3` (cwd: /home/cedric/USBCellular/esp32/)
- Flash command: `~/.local/bin/pio run -e esp32s3 -t upload --upload-port /dev/ttyACMX`
- PIO binary: ~/.local/bin/pio
- esptool: ~/.platformio/packages/tool-esptoolpy/esptool.py

## Firmware Purpose
USB MSC (mass storage) + CDC (serial debug) composite device using TinyUSB.
SD card on HSPI: CS=10, MOSI=11, MISO=12, SCK=13
OLED SSD1306 on I2C: SDA=8, SCL=7

## TinyUSB CDC + Flashing: Critical Notes
See tinyusb-cdc-notes.md for full details.

### Flashing when app is running (TinyUSB mode, NOT USB-Serial-JTAG ROM):
- esptool cannot auto-reset a TinyUSB CDC device — RTS does not reach EN pin
- Use 1200-baud magic reset: open port at 1200 baud and close it (NO DTR manipulation)
  - Setting p.dtr = False raises OSError: [Errno 71] Protocol error on TinyUSB CDC
  - Baud-rate change alone is sufficient to trigger the Arduino bootloader reset handler
  - python3 -c "import serial, time; p = serial.Serial('/dev/ttyACM0', 1200); time.sleep(0.5); p.close()"
  → device enters ROM USB-Serial-JTAG bootloader; lsusb shows "Espressif USB JTAG/serial debug unit"
  → port may stay as /dev/ttyACM0 (not necessarily a new number) — check lsusb to confirm mode
  → immediately run: pio run -e esp32s3 -t upload --upload-port /dev/ttyACM0
- CAUTION: if nothing connects after 1200-baud reset, device stays in ROM
  bootloader indefinitely. Physical RESET needed to recover.

### After flashing (esptool hard_reset):
- Device runs app immediately; CDC port re-enumerates at /dev/ttyACM0
- Boot messages are lost unless USB.begin() is called BEFORE Serial.begin()
  and a 3s wait loop is used: `while (!DBG && (millis()-t0) < 3000) delay(10);`

## SdFat API (v2.3.x)
- sectorCount() returns value directly: `sd_sectors = sd.card()->sectorCount();`
- Old pointer form `sectorCount(&sd_sectors)` broke in 2.3.x (was valid in 2.2.x)

## Serial Output Capture Strategy
Best approach for TinyUSB CDC:
1. Start reader thread watching glob("/dev/ttyACM*")
2. Open each port as soon as it appears (DTR manipulation raises Protocol error — skip it)
3. Trigger reset via 1200-baud magic (NOT RTS or DTR — neither works on TinyUSB CDC)
4. Read output; firmware must have USB.begin() before boot prints

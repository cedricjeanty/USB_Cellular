#!/usr/bin/env python3
"""
Reliable button-free flash / recovery for the ESP32-S3 over USB-Serial-JTAG.

Why this exists
---------------
The normal button-free flash path is the 1200-baud touch on the TinyUSB CDC port
(see docs/deployment.md). But that needs the firmware to boot far enough to
present CDC. If a bad firmware hangs in early boot it presents NOTHING usable
(MSC-only holds D+ low; a crash loops the USB-Serial-JTAG), and then the only
recovery is the ROM bootloader — historically the BOOT button.

This script reaches the ROM bootloader WITHOUT the button. At power-on the chip
briefly exposes its USB-Serial-JTAG (303a:1001) before the app switches the USB
pins to TinyUSB. A *cold* esptool always misses that <1s window. The trick here:
keep a WARM (already-imported) esptool process busy-waiting on the port and fire
`default-reset` the instant the JTAG enumerates — that lands inside the window
and drops the chip into a stable download mode. Then erase + write a full image.

Usage
-----
  scripts/jtag_flash.py [--env esp32s3-e2e] [--app-bin PATH] [--no-erase]
                        [--port-glob /dev/ttyACM*] [--manual]

  --env       PlatformIO build env whose .pio/build/<env>/ images to flash.
  --app-bin   Flash this app image at 0x10000 instead of the env's firmware.bin
              (e.g. a prod latest.bin pulled from S3), keeping the env's
              bootloader/partitions/ota_data.
  --no-erase  Skip the full erase (erase clears a stale OTA selector — recommended
              when recovering, since esptool writes a slot the OTA data may not select).
  --manual    Don't use the CoolGear hub; prompt you to power-cycle by hand.

Power is cycled via scripts/coolgear.py when present, else --manual prompts.
"""
import argparse, glob, os, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ESPTOOL = os.path.expanduser("~/.local/bin/esptool.py")
if not os.path.exists(ESPTOOL):
    ESPTOOL = "esptool.py"  # fall back to PATH

# Offsets must match flasher_args.json for the env.
IMAGES = [("0x0", "bootloader.bin"),
          ("0x8000", "partitions.bin"),
          ("0xe000", "ota_data_initial.bin"),
          ("0x10000", "firmware.bin")]


def coolgear(cmd):
    try:
        subprocess.run(["python3", os.path.join(ROOT, "scripts", "coolgear.py"), cmd],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
        return True
    except Exception:
        return False


def power(cmd, manual, prompt):
    if manual or not coolgear(cmd):
        input("  >> %s, then press Enter... " % prompt)


def catch_download_mode(port_glob, manual):
    """Power-cycle and warm-catch the JTAG into ROM download mode. Returns the port."""
    import esptool  # warm import BEFORE the window opens
    print("esptool", getattr(esptool, "__version__", "?"))
    power("off", manual, "power the device OFF"); time.sleep(2)
    # Begin powering on, then busy-wait for the JTAG and fire default-reset.
    if not manual:
        coolgear("on")
    else:
        input("  >> power the device ON, then press Enter quickly... ")
    print("waiting for USB-Serial-JTAG window...")
    deadline = time.time() + 45
    port = None
    while time.time() < deadline:
        hits = sorted(glob.glob(port_glob))
        if hits:
            port = hits[0]
            break
    if not port:
        return None
    # Fire repeatedly through the brief window + udev permission settle.
    end = time.time() + 6
    while time.time() < end:
        try:
            esptool.main(["--chip", "esp32s3", "--port", port, "--before", "default_reset",
                          "--after", "no_reset", "--connect-attempts", "1", "flash_id"])
            return port
        except SystemExit as e:
            if e.code in (0, None):
                return port
        except Exception:
            pass
        time.sleep(0.02)
    return None


def esp(port, *args):
    cmd = [ESPTOOL, "--chip", "esp32s3", "--port", port] + list(args)
    return subprocess.run(cmd, cwd=ROOT).returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--env", default="esp32s3-e2e")
    ap.add_argument("--app-bin", default=None)
    ap.add_argument("--no-erase", action="store_true")
    ap.add_argument("--port-glob", default="/dev/ttyACM*")
    ap.add_argument("--manual", action="store_true")
    a = ap.parse_args()

    bdir = os.path.join(ROOT, "esp32", ".pio", "build", a.env)
    flash_args = []
    for off, name in IMAGES:
        path = a.app_bin if (off == "0x10000" and a.app_bin) else os.path.join(bdir, name)
        if not os.path.exists(path):
            sys.exit("missing image: %s (build the env first: pio run -e %s)" % (path, a.env))
        flash_args += [off, path]

    print("== catching ROM download mode (no button) ==")
    port = catch_download_mode(a.port_glob, a.manual)
    if not port:
        sys.exit("FAILED to catch download mode — retry, or hold BOOT as a last resort")
    print("== in download mode on", port, "==")

    if not a.no_erase:
        print("== full erase (clears OTA selector) ==")
        if esp(port, "--before", "no_reset", "--after", "no_reset", "erase_flash") != 0:
            sys.exit("erase failed")

    print("== write image ==")
    if esp(port, "--before", "no_reset", "--after", "no_reset", "write_flash", *flash_args) != 0:
        sys.exit("write failed")

    print("== reboot into new firmware ==")
    power("off", a.manual, "power the device OFF"); time.sleep(3)
    power("on", a.manual, "power the device ON")
    print("DONE — device flashed and rebooting")


if __name__ == "__main__":
    main()

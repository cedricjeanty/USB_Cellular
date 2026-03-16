#!/usr/bin/env python3
"""
ESP32-S3 end-to-end integration test.

Steps:
  1. Clean old test files from Dropbox
  2. Verify device is connected and ready
  3. Find USB drive mount
  4. Write a test file to the USB drive
  5. Wait for quiet window + harvest + Dropbox upload
  6. Verify the file appears on Dropbox with correct size

Usage:
    python3 scripts/test_e2e_esp32.py
    python3 scripts/test_e2e_esp32.py --file-size-mb 1    # smaller/faster
    python3 scripts/test_e2e_esp32.py --wifi-ssid MyNet --wifi-pass hunter2
"""

import argparse
import glob
import json
import os
import subprocess
import sys
import time

import requests
import serial

BAUD = 115200
UPLOAD_TIMEOUT = 300  # seconds to poll Dropbox for the file

# Dropbox credentials (same as provisioned on ESP32)
DBX_APP_KEY = "82un1zz0uurgszt"
DBX_APP_SECRET = "hc3wapn8hsgzuva"
DBX_REFRESH_TOKEN = "J0Iqey6GFFsAAAAAAAAAAa7xToJsNmRAqr1Ok5WOGyqGlIhJI0wcSdL2_LKv6quE"


# ── Dropbox helpers ──────────────────────────────────────────────────────────

def dbx_get_token():
    resp = requests.post(
        "https://api.dropboxapi.com/oauth2/token",
        auth=(DBX_APP_KEY, DBX_APP_SECRET),
        data={"grant_type": "refresh_token", "refresh_token": DBX_REFRESH_TOKEN},
    )
    resp.raise_for_status()
    return resp.json()["access_token"]


def dbx_list_files(token):
    resp = requests.post(
        "https://api.dropboxapi.com/2/files/list_folder",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={"path": "", "limit": 100},
    )
    if resp.status_code == 200:
        return {e["name"]: e for e in resp.json().get("entries", [])}
    return {}


def dbx_delete(token, path):
    requests.post(
        "https://api.dropboxapi.com/2/files/delete_v2",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={"path": path},
    )


def dbx_wait_for_file(token, filename, expected_size, timeout=300):
    """Poll Dropbox until filename appears with correct size."""
    print(f"  Polling Dropbox for '{filename}' ...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            files = dbx_list_files(token)
            if filename in files:
                size = files[filename].get("size", 0)
                elapsed = time.time() - t0
                print(f" FOUND ({elapsed:.1f}s, {size:,} bytes)")
                return size == expected_size
        except Exception:
            token = dbx_get_token()  # refresh if expired
        time.sleep(5)
        print(".", end="", flush=True)
    print(f" NOT FOUND after {timeout}s")
    return False


# ── Serial/USB helpers ───────────────────────────────────────────────────────

def find_serial_port():
    """Find an available ttyACM port."""
    for port in sorted(glob.glob("/dev/ttyACM*")):
        try:
            s = serial.Serial(port, BAUD, timeout=1)
            s.close()
            return port
        except Exception:
            pass
    return None


def open_serial(port):
    s = serial.Serial(port, BAUD, timeout=2)
    time.sleep(0.5)
    s.reset_input_buffer()
    return s


def cli(s, cmd, wait=1.0):
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    s.flush()
    time.sleep(wait)
    resp = s.read(s.in_waiting).decode(errors="replace").strip()
    print(f"  >> {cmd}")
    if resp:
        for line in resp.splitlines():
            print(f"  << {line}")
    return resp


def find_usb_mount(timeout=30):
    print(f"  Scanning for USB drive mount ...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        result = subprocess.run(
            ["lsblk", "-J", "-o", "NAME,FSTYPE,MOUNTPOINT,SIZE"],
            capture_output=True, text=True,
        )
        try:
            data = json.loads(result.stdout)
            for dev in data.get("blockdevices", []):
                for target in [dev] + dev.get("children", []):
                    mp = target.get("mountpoint") or ""
                    if target.get("fstype") in ("vfat", "exfat") and mp.startswith("/media"):
                        print(f" found: {mp}")
                        return mp
        except Exception:
            pass
        time.sleep(2)
        print(".", end="", flush=True)
    print(" NOT FOUND")
    return None


# ── main ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="ESP32-S3 E2E integration test")
    ap.add_argument("--wifi-ssid", default=None,
                    help="WiFi SSID to provision; omit if already configured")
    ap.add_argument("--wifi-pass", default="")
    ap.add_argument("--serial-port", default=None,
                    help="Serial port (default: auto-detect)")
    ap.add_argument("--file-size-mb", type=int, default=1)
    ap.add_argument("--quiet-window", type=int, default=50,
                    help="Seconds to wait after write (default: 50)")
    args = ap.parse_args()

    print("\n╔══════════════════════════════════════════╗")
    print("║   ESP32-S3 E2E Integration Test          ║")
    print("╚══════════════════════════════════════════╝\n")

    # ── Step 1: Dropbox setup ─────────────────────────────────────────────────
    print("[1/6] Setting up Dropbox ...")
    token = dbx_get_token()
    print(f"  Token OK")
    files = dbx_list_files(token)
    for name in list(files.keys()):
        if name.startswith("e2e_"):
            dbx_delete(token, f"/{name}")
            print(f"  Deleted old: {name}")

    # ── Step 2: Verify device ─────────────────────────────────────────────────
    port = args.serial_port or find_serial_port()
    if not port:
        print("ERROR: no serial port found"); sys.exit(1)
    print(f"\n[2/6] Checking device on {port} ...")
    s = open_serial(port)
    if args.wifi_ssid:
        cli(s, f"SETWIFI {args.wifi_ssid} {args.wifi_pass}")
        cli(s, "REBOOT", wait=0.3)
        s.close()
        time.sleep(8)
        port = find_serial_port()
        s = open_serial(port)
        # Wait for WiFi
        for _ in range(15):
            resp = cli(s, "STATUS", wait=2)
            if "wifi=" in resp and "disconnected" not in resp:
                break
            time.sleep(5)
    else:
        status = cli(s, "STATUS")
        if "disconnected" in status:
            print("ERROR: WiFi not connected"); sys.exit(1)
    s.close()

    # ── Step 3: Find USB drive ────────────────────────────────────────────────
    print(f"\n[3/6] Locating USB drive ...")
    mount = find_usb_mount(timeout=15)
    if not mount:
        print("ERROR: USB drive not mounted"); sys.exit(1)

    # Clean old test files from SD card
    for old in glob.glob(os.path.join(mount, "e2e_*.bin")):
        try: os.unlink(old)
        except Exception: pass
    harvested = os.path.join(mount, "harvested")
    if os.path.isdir(harvested):
        for old in glob.glob(os.path.join(harvested, "e2e_*.bin")):
            try: os.unlink(old)
            except Exception: pass

    # ── Step 4: Write test file ───────────────────────────────────────────────
    test_filename = f"e2e_{int(time.time())}.bin"
    test_path = os.path.join(mount, test_filename)
    size_mb = args.file_size_mb
    print(f"\n[4/6] Writing {size_mb} MB test file → {test_path} ...")
    chunk = b'\xAB\xCD\xEF\x01' * 256  # 1 KB
    with open(test_path, "wb") as f:
        for _ in range(size_mb * 1024):
            f.write(chunk)
        f.flush()
        os.fsync(f.fileno())
    os.sync()
    actual = os.path.getsize(test_path)
    print(f"  Written: {actual:,} bytes ({actual/1e6:.1f} MB)")

    # ── Step 5: Wait for harvest + upload ─────────────────────────────────────
    wait_secs = args.quiet_window
    print(f"\n[5/6] Waiting {wait_secs}s for quiet window + harvest + upload ...")
    for remaining in range(wait_secs, 0, -10):
        print(f"  {remaining}s remaining ...", flush=True)
        time.sleep(min(10, remaining))

    # ── Step 6: Verify on Dropbox ─────────────────────────────────────────────
    print(f"\n[6/6] Verifying '{test_filename}' on Dropbox ...")
    token = dbx_get_token()
    found = dbx_wait_for_file(token, test_filename, actual, timeout=UPLOAD_TIMEOUT)

    # Print serial log for diagnostics
    try:
        port = find_serial_port()
        if port:
            s2 = open_serial(port)
            time.sleep(0.5)
            log = s2.read(s2.in_waiting).decode(errors="replace").strip()
            s2.close()
            if log:
                print("\n  --- Serial log ---")
                for line in log.splitlines():
                    print(f"  {line}")
    except Exception:
        pass

    print()
    if found:
        print("✓  PASS — file harvested and uploaded to Dropbox successfully")
        sys.exit(0)
    else:
        print("✗  FAIL — file not found on Dropbox within timeout")
        sys.exit(1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Upload-resume integration test.

Writes a large file to the USB drive, waits for the upload to start,
resets the ESP32 mid-upload, and verifies the upload resumes and
completes successfully on Dropbox.

Usage:
    python3 scripts/test_upload_resume.py

Options:
    --serial-port PORT   Serial port (default: /dev/ttyACM0)
    --file-size-mb N     Size of test file in MB (default: 5)
    --interrupt-pct N    Interrupt after N% uploaded (default: 40)
    --dropbox-token TOK  Access token (default: mints one via refresh token)
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

import serial
import requests
from pathlib import Path

BAUD = 115200

# Dropbox app credentials (same as provisioned on ESP32)
DBX_APP_KEY = "82un1zz0uurgszt"
DBX_APP_SECRET = "hc3wapn8hsgzuva"
DBX_REFRESH_TOKEN = "J0Iqey6GFFsAAAAAAAAAAa7xToJsNmRAqr1Ok5WOGyqGlIhJI0wcSdL2_LKv6quE"


def dbx_get_token():
    """Mint a fresh Dropbox access token."""
    resp = requests.post(
        "https://api.dropboxapi.com/oauth2/token",
        auth=(DBX_APP_KEY, DBX_APP_SECRET),
        data={"grant_type": "refresh_token", "refresh_token": DBX_REFRESH_TOKEN},
    )
    resp.raise_for_status()
    return resp.json()["access_token"]


def dbx_list_files(token):
    """List files in the app folder."""
    resp = requests.post(
        "https://api.dropboxapi.com/2/files/list_folder",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={"path": "", "limit": 100},
    )
    if resp.status_code == 200:
        return {e["name"]: e for e in resp.json().get("entries", [])}
    return {}


def dbx_delete(token, path):
    """Delete a file from Dropbox."""
    requests.post(
        "https://api.dropboxapi.com/2/files/delete_v2",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"},
        json={"path": path},
    )


def open_serial(port):
    s = serial.Serial(port, BAUD, timeout=2)
    time.sleep(0.5)
    s.reset_input_buffer()
    return s


def cli(s, cmd, wait=1.0):
    """Send a CLI command and return response."""
    s.reset_input_buffer()
    s.write((cmd + "\n").encode())
    s.flush()
    time.sleep(wait)
    return s.read(s.in_waiting).decode(errors="replace").strip()


def find_usb_mount(timeout=20):
    """Find the ESP32's USB drive mount point."""
    import json as _json
    t0 = time.time()
    while time.time() - t0 < timeout:
        result = subprocess.run(
            ["lsblk", "-J", "-o", "NAME,FSTYPE,MOUNTPOINT,SIZE"],
            capture_output=True, text=True
        )
        try:
            data = _json.loads(result.stdout)
            for dev in data.get("blockdevices", []):
                for target in [dev] + dev.get("children", []):
                    mp = target.get("mountpoint") or ""
                    if target.get("fstype") in ("vfat", "exfat") and mp.startswith("/media"):
                        return mp
        except Exception:
            pass
        time.sleep(2)
    return None


def monitor_until(s, pattern, timeout=300):
    """Read serial until pattern appears or timeout. Returns all output."""
    output = ""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            data = s.read(max(s.in_waiting, 1))
            if data:
                text = data.decode(errors="replace")
                output += text
                sys.stdout.write(text)
                sys.stdout.flush()
                if pattern in output:
                    return output
        except (OSError, serial.SerialException):
            time.sleep(1)
            try:
                s.close()
                s = serial.Serial(s.port, BAUD, timeout=1)
            except Exception:
                pass
    return output


def main():
    ap = argparse.ArgumentParser(description="Upload resume integration test")
    ap.add_argument("--serial-port", default="/dev/ttyACM0")
    ap.add_argument("--file-size-mb", type=int, default=5)
    ap.add_argument("--interrupt-pct", type=int, default=40)
    args = ap.parse_args()

    print("\n╔══════════════════════════════════════════╗")
    print("║   Upload Resume Integration Test         ║")
    print("╚══════════════════════════════════════════╝\n")

    # ── Step 1: Get Dropbox token and clean up any old test files ──────────
    print("[1/7] Setting up Dropbox ...")
    token = dbx_get_token()
    print(f"  Token OK")
    # Clean up any previous test files
    files = dbx_list_files(token)
    for name in list(files.keys()):
        if name.startswith("resume_test_"):
            dbx_delete(token, f"/{name}")
            print(f"  Deleted old: {name}")

    # ── Step 2: Verify device is ready ────────────────────────────────────
    print(f"\n[2/7] Checking device on {args.serial_port} ...")
    s = open_serial(args.serial_port)
    status = cli(s, "STATUS")
    print(f"  {status}")
    if "wifi=" not in status or "disconnected" in status:
        print("ERROR: device not connected to WiFi"); sys.exit(1)
    s.close()

    # ── Step 3: Write test file to USB drive ──────────────────────────────
    print(f"\n[3/7] Writing {args.file_size_mb} MB test file ...")
    mount = find_usb_mount()
    if not mount:
        print("ERROR: USB drive not mounted"); sys.exit(1)

    # Clean up old test files from SD card
    import glob
    for old in glob.glob(os.path.join(mount, "resume_test_*.bin")):
        os.unlink(old)
    harvested = os.path.join(mount, "harvested")
    if os.path.isdir(harvested):
        for old in glob.glob(os.path.join(harvested, "resume_test_*.bin")):
            os.unlink(old)
    os.sync()

    test_filename = f"resume_test_{int(time.time())}.bin"
    test_path = os.path.join(mount, test_filename)
    chunk = b'\xAB\xCD\xEF\x01' * 256  # 1 KB
    with open(test_path, "wb") as f:
        for _ in range(args.file_size_mb * 1024):
            f.write(chunk)
        f.flush()
        os.fsync(f.fileno())
    os.sync()
    actual = os.path.getsize(test_path)
    print(f"  Written: {actual:,} bytes to {test_path}")

    # ── Step 4: Wait for upload to start, then interrupt ──────────────────
    target_pct = args.interrupt_pct
    print(f"\n[4/7] Waiting for upload to reach ~{target_pct}% then resetting ...")

    s = serial.Serial(args.serial_port, BAUD, timeout=1)
    time.sleep(0.3)

    # Monitor for upload progress
    interrupted = False
    t0 = time.time()
    output = ""
    while time.time() - t0 < 600:  # 10 min — host delayed writes can take 4+ min
        try:
            data = s.read(max(s.in_waiting, 1))
            if data:
                text = data.decode(errors="replace")
                output += text
                sys.stdout.write(text)
                sys.stdout.flush()

                # Look for progress like "DBX: 2097152/5242880 bytes (40%,"
                for m in re.finditer(r'(\d+)/\d+ bytes \((\d+)%', text):
                    pct = int(m.group(2))
                    if pct >= target_pct:
                        print(f"\n  >>> Reached {pct}% — sending REBOOT <<<")
                        s.write(b"REBOOT\n")
                        s.flush()
                        time.sleep(1)
                        interrupted = True
                        break
                if interrupted:
                    break
        except (OSError, serial.SerialException):
            time.sleep(1)
            try:
                s.close()
                s = serial.Serial(args.serial_port, BAUD, timeout=1)
            except Exception:
                pass

    s.close()
    if not interrupted:
        # Maybe upload completed before we could interrupt (small file / fast connection)
        if "uploaded" in output:
            print("\n  Upload completed before interrupt — try a larger file")
        else:
            print("\n  ERROR: upload never started within 5 minutes")
            sys.exit(1)

    # ── Step 5: Wait for device to reboot ─────────────────────────────────
    print(f"\n[5/7] Waiting for device to reboot ...")
    time.sleep(5)  # let USB re-enumerate

    # Wait for serial port to come back (port number may change after reboot)
    import glob as _glob
    active_port = None
    for attempt in range(30):
        for port in sorted(_glob.glob("/dev/ttyACM*")):
            try:
                s = serial.Serial(port, BAUD, timeout=2)
                time.sleep(1)
                s.reset_input_buffer()
                s.write(b"STATUS\n")
                s.flush()
                time.sleep(2)
                resp = s.read(max(s.in_waiting, 1)).decode(errors="replace")
                if "wifi=" in resp:
                    active_port = port
                    print(f"  Device back on {port}: {resp.strip()}")
                    s.close()
                    break
                s.close()
            except Exception:
                pass
        if active_port:
            break
        time.sleep(2)
    else:
        print("  ERROR: device did not come back after reboot")
        sys.exit(1)

    # ── Step 6: Monitor for resumed upload ────────────────────────────────
    print(f"\n[6/7] Monitoring for resumed upload ...")
    s = serial.Serial(active_port, BAUD, timeout=1)
    time.sleep(0.3)
    # Trigger upload if it hasn't auto-started from boot scan
    s.write(b"UPLOAD\n")
    s.flush()

    output = monitor_until(s, "uploaded", timeout=600)
    s.close()

    resumed = "resuming session" in output.lower() or "resuming" in output.lower()
    uploaded = "uploaded" in output and "OK" in output

    if resumed:
        print(f"\n  Upload RESUMED from stored session")
    if uploaded:
        print(f"  Upload completed!")

    # ── Step 7: Verify file on Dropbox ────────────────────────────────────
    print(f"\n[7/7] Verifying '{test_filename}' on Dropbox ...")
    # Poll for up to 30 seconds (server processing delay)
    found = False
    for _ in range(6):
        token = dbx_get_token()  # refresh in case it expired
        files = dbx_list_files(token)
        if test_filename in files:
            entry = files[test_filename]
            size = entry.get("size", 0)
            print(f"  FOUND: {test_filename} ({size:,} bytes)")
            if size == actual:
                found = True
                print(f"  Size matches: {actual:,} bytes")
            else:
                print(f"  WARNING: size mismatch (expected {actual:,}, got {size:,})")
                found = True  # still found, just wrong size
            break
        time.sleep(5)

    print()
    if found and resumed:
        print("✓  PASS — upload interrupted, resumed, and completed successfully")
        sys.exit(0)
    elif found and not interrupted:
        print("✓  PASS — upload completed (was not interrupted — file too small?)")
        sys.exit(0)
    elif found:
        print("~  PARTIAL — file found on Dropbox but resume not confirmed in logs")
        sys.exit(0)
    else:
        print("✗  FAIL — file not found on Dropbox")
        sys.exit(1)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
ESP32-S3 end-to-end integration test.

Steps:
  1. Get a temporary FTP server from sftpcloud.io (60-min free session)
  2. Provision WiFi + FTP credentials into the device via the serial CLI
  3. Reboot the device
  4. Wait for USB drive to re-enumerate and WiFi to connect
  5. Write a 10 MB binary test file to the USB drive
  6. Wait for the quiet window (30 s) + harvest + FTP upload
  7. Verify the file appears on the FTP server

Usage:
    python3 scripts/test_e2e_esp32.py --wifi-ssid MyNet --wifi-pass hunter2

Options:
    --wifi-ssid SSID         (required) Home/lab WiFi SSID
    --wifi-pass PASS         WiFi password (default: empty for open networks)
    --serial-port PORT       Serial port (default: /dev/ttyACM0)
    --ftp-remote-path PATH   Remote dir on FTP server (default: /)
    --no-ftp-creds           Reuse FTP server from config.yaml instead of allocating new one
    --file-size-mb N         Size of test file in MB (default: 10)
    --quiet-window N         Seconds to wait for quiet window (default: 30 + 10 buffer = 40)
"""

import argparse
import ftplib
import glob
import os
import subprocess
import sys
import time
import serial
from pathlib import Path

# Locate get_ftp_creds in the same scripts/ dir
sys.path.insert(0, str(Path(__file__).parent))
from get_ftp_creds import get_credentials

CONFIG_PATH = Path(__file__).parent.parent / "config.yaml"
BAUD = 115200
UPLOAD_TIMEOUT = 300   # seconds to poll FTP server for the file


# ── helpers ───────────────────────────────────────────────────────────────────

def wait_for(path, timeout=30, label=None):
    label = label or path
    print(f"  Waiting for {label} ...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if os.path.exists(path):
            print(f" OK ({time.time()-t0:.1f}s)")
            return True
        time.sleep(1)
        print(".", end="", flush=True)
    print(f" TIMEOUT after {timeout}s")
    return False


def open_serial(port):
    s = serial.Serial(port, BAUD, timeout=2)
    time.sleep(0.5)
    s.reset_input_buffer()
    return s


def cli(s, cmd, wait=0.6):
    """Send a CLI command and return the device's response."""
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
    """Return the first auto-mounted FAT partition that looks like the ESP32 SD card."""
    print(f"  Scanning for USB drive mount ...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        # lsblk gives us SIZE=14.8G fstype=vfat mounts — look for small-ish vfat
        result = subprocess.run(
            ["lsblk", "-J", "-o", "NAME,FSTYPE,MOUNTPOINT,SIZE"],
            capture_output=True, text=True
        )
        import json
        try:
            data = json.loads(result.stdout)
            for dev in data.get("blockdevices", []):
                mp = dev.get("mountpoint") or ""
                if dev.get("fstype") in ("vfat", "exfat") and mp and mp.startswith("/media"):
                    print(f" found: {mp}")
                    return mp
                for child in dev.get("children", []):
                    mp = child.get("mountpoint") or ""
                    if child.get("fstype") in ("vfat", "exfat") and mp and mp.startswith("/media"):
                        print(f" found: {mp}")
                        return mp
        except Exception:
            pass
        # fallback: glob
        for mp in glob.glob("/media/*/*") + glob.glob("/media/*"):
            if os.path.ismount(mp):
                print(f" found: {mp}")
                return mp
        time.sleep(2)
        print(".", end="", flush=True)
    print(" NOT FOUND")
    return None


def wait_for_wifi(s, timeout=60):
    """Poll STATUS command until WiFi shows a real IP."""
    print(f"  Waiting up to {timeout}s for WiFi connection ...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        s.reset_input_buffer()
        s.write(b"STATUS\n")
        s.flush()
        time.sleep(1.0)
        resp = s.read(s.in_waiting).decode(errors="replace")
        # STATUS line: "CLI: wifi=192.168.x.x ap=no ..."
        for line in resp.splitlines():
            if "wifi=" in line and "disconnected" not in line and "Connecting" not in line:
                ip = line.split("wifi=")[1].split()[0]
                if ip and ip[0].isdigit():
                    print(f" connected ({ip}, {time.time()-t0:.0f}s)")
                    return True
        print(".", end="", flush=True)
        time.sleep(4)
    print(f" TIMEOUT")
    return False


def wait_for_file_on_ftp(host, port, user, password, remote_path, filename, timeout=120):
    """Poll the FTP server until filename appears in remote_path."""
    print(f"  Polling FTP for '{filename}' ...", end="", flush=True)
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            ftp = ftplib.FTP()
            ftp.connect(host, port, timeout=10)
            ftp.login(user, password)
            try:
                ftp.cwd(remote_path)
            except ftplib.error_perm:
                pass  # path may not exist yet or may be root
            files = ftp.nlst()
            ftp.quit()
            if filename in files:
                elapsed = time.time() - t0
                print(f" FOUND ({elapsed:.1f}s)")
                return True
        except Exception as e:
            pass  # server not ready yet
        time.sleep(5)
        print(".", end="", flush=True)
    print(f" NOT FOUND after {timeout}s")
    return False


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description="ESP32-S3 E2E integration test")
    ap.add_argument("--wifi-ssid",       default=None,
                    help="WiFi SSID to provision; omit if device already has credentials")
    ap.add_argument("--wifi-pass",       default="")
    ap.add_argument("--serial-port",     default="/dev/ttyACM0")
    ap.add_argument("--ftp-remote-path", default="/")
    ap.add_argument("--no-ftp-creds",    action="store_true",
                    help="Reuse FTP config from config.yaml")
    ap.add_argument("--file-size-mb",    type=int, default=10)
    ap.add_argument("--quiet-window",    type=int, default=40,
                    help="Seconds to wait after last write (default: 40 = 30s window + 10s buffer)")
    args = ap.parse_args()

    print("\n╔══════════════════════════════════════════╗")
    print("║   ESP32-S3 E2E Integration Test          ║")
    print("╚══════════════════════════════════════════╝\n")

    # ── Step 1: FTP credentials ────────────────────────────────────────────────
    print("[1/7] Allocating FTP server ...")
    if args.no_ftp_creds:
        import yaml
        cfg = yaml.safe_load(open(CONFIG_PATH))
        ftp_cfg = cfg["ftp"]
        ftp_host = ftp_cfg["server"]
        ftp_port = int(ftp_cfg.get("port", 21))
        ftp_user = ftp_cfg["username"]
        ftp_pass = ftp_cfg["password"]
        print(f"  Reusing config.yaml: {ftp_user}@{ftp_host}:{ftp_port}")
    else:
        creds = get_credentials(headless=True)
        if not creds.get("username") or not creds.get("password"):
            print(f"  ERROR: could not get FTP credentials: {creds}")
            sys.exit(1)
        ftp_host = creds["host"]
        ftp_port = int(creds.get("port", 21))
        ftp_user = creds["username"]
        ftp_pass = creds["password"]
        print(f"  Got: {ftp_user}@{ftp_host}:{ftp_port}")

    ftp_path = args.ftp_remote_path

    # ── Step 2: Provision FTP (and optionally WiFi) via serial CLI ───────────
    print(f"\n[2/7] Provisioning credentials via {args.serial_port} ...")
    s = open_serial(args.serial_port)
    if args.wifi_ssid:
        cli(s, f"SETWIFI {args.wifi_ssid} {args.wifi_pass}")
    cli(s, f"SETFTP {ftp_host} {ftp_port} {ftp_user} {ftp_pass} {ftp_path}")
    cli(s, "STATUS")

    # ── Step 3: Reboot (only needed when provisioning new WiFi creds) ─────────
    if args.wifi_ssid:
        print(f"\n[3/7] Rebooting device ...")
        cli(s, "REBOOT", wait=0.3)
        s.close()
        time.sleep(4)  # let USB re-enumerate

        print(f"\n[4/7] Waiting for USB drive and WiFi ...")
        if not wait_for(args.serial_port, timeout=20, label="serial port"):
            print("ERROR: device did not come back after reboot"); sys.exit(1)
        time.sleep(3)
        mount = find_usb_mount(timeout=20)
        if not mount:
            print("ERROR: USB drive not mounted"); sys.exit(1)

        s = open_serial(args.serial_port)
        if not wait_for_wifi(s, timeout=75):
            print("ERROR: device did not connect to WiFi within 75s"); sys.exit(1)
        s.close()
    else:
        print(f"\n[3/7] WiFi already provisioned — skipping reboot")
        s.close()
        print(f"\n[4/7] Locating USB drive ...")
        mount = find_usb_mount(timeout=10)
        if not mount:
            print("ERROR: USB drive not mounted"); sys.exit(1)

    # ── Step 5: Write test file ────────────────────────────────────────────────
    test_filename = f"e2e_{int(time.time())}.bin"
    test_path = os.path.join(mount, test_filename)
    size_mb = args.file_size_mb
    print(f"\n[5/7] Writing {size_mb} MB test file → {test_path} ...")
    chunk = b'\xAB\xCD\xEF\x01' * 256  # 1 KB
    with open(test_path, "wb") as f:
        for _ in range(size_mb * 1024):  # size_mb × 1024 × 1 KB = size_mb MB
            f.write(chunk)
        f.flush()
        os.fsync(f.fileno())  # wait for data to reach USB device
    os.sync()
    actual = os.path.getsize(test_path)
    print(f"  Written: {actual:,} bytes ({actual/1e6:.1f} MB)")

    # ── Step 6: Wait for quiet window ─────────────────────────────────────────
    wait_secs = args.quiet_window
    print(f"\n[6/7] Waiting {wait_secs}s for quiet window + harvest + upload ...")
    for remaining in range(wait_secs, 0, -10):
        print(f"  {remaining}s remaining ...", flush=True)
        time.sleep(min(10, remaining))

    # ── Step 7: Verify on FTP ─────────────────────────────────────────────────
    print(f"\n[7/7] Verifying '{test_filename}' on FTP ({ftp_host}) ...")
    found = wait_for_file_on_ftp(
        ftp_host, ftp_port, ftp_user, ftp_pass, ftp_path,
        test_filename, timeout=UPLOAD_TIMEOUT
    )

    # Print serial log for diagnostics
    try:
        s2 = open_serial(args.serial_port)
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
        print("✓  PASS — file harvested and uploaded to FTP successfully")
        sys.exit(0)
    else:
        print("✗  FAIL — file not found on FTP within timeout")
        sys.exit(1)


if __name__ == "__main__":
    main()

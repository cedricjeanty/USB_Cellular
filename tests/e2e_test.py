#!/usr/bin/env python3
"""
End-to-End Test: USB Drive -> File Harvest -> FTP Upload
Tests the complete Airbridge pipeline.
"""

import os
import sys
import time
import yaml
import subprocess
import zipfile
from datetime import datetime

# Load configuration
with open("config.yaml", "r") as ymlfile:
    cfg = yaml.safe_load(ymlfile)

from modem_handler import SIM7000GHandler

def run_cmd(cmd, check=True):
    """Run a shell command and return output."""
    print(f"$ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout.strip())
    if result.stderr:
        print(result.stderr.strip())
    if check and result.returncode != 0:
        print(f"Command failed with code {result.returncode}")
        return False
    return True

def phase1_usb_gadget():
    """Phase 1: Test USB gadget loading/unloading."""
    print("\n" + "="*50)
    print("PHASE 1: USB Gadget Test")
    print("="*50)

    disk_path = cfg['virtual_disk_path']

    # Check if disk/file exists
    if not os.path.exists(disk_path):
        print(f"ERROR: Virtual disk not found: {disk_path}")
        return False

    # Unload any existing gadget first
    print("\nUnloading any existing USB gadget...")
    run_cmd(["sudo", "modprobe", "-r", "g_mass_storage"], check=False)
    time.sleep(1)

    # Load the gadget
    print("\nLoading USB mass storage gadget...")
    if not run_cmd(["sudo", "modprobe", "g_mass_storage",
                    f"file={disk_path}", "stall=0", "removable=1"]):
        return False

    time.sleep(2)

    # Check UDC state
    print("\nChecking UDC state...")
    udc_dirs = os.listdir("/sys/class/udc/")
    if udc_dirs:
        state_file = f"/sys/class/udc/{udc_dirs[0]}/state"
        with open(state_file, 'r') as f:
            state = f.read().strip()
        print(f"UDC state: {state}")

    print("\nPhase 1 PASSED: USB gadget loaded successfully")
    return True

def phase2_harvest():
    """Phase 2: Unload gadget, mount disk, create test file, harvest."""
    print("\n" + "="*50)
    print("PHASE 2: Log Harvesting Test")
    print("="*50)

    disk_path = cfg['virtual_disk_path']
    mount_point = cfg['mount_point']
    outbox_dir = cfg['outbox_dir']

    # Create outbox if needed
    os.makedirs(outbox_dir, exist_ok=True)

    # Unload USB gadget
    print("\nUnloading USB gadget for local access...")
    run_cmd(["sudo", "modprobe", "-r", "g_mass_storage"], check=False)
    time.sleep(1)

    # Ensure mount point exists
    os.makedirs(mount_point, exist_ok=True)

    # Mount the disk locally
    print(f"\nMounting {disk_path} to {mount_point}...")

    # Unmount if already mounted
    run_cmd(["sudo", "umount", mount_point], check=False)
    time.sleep(0.5)

    # Try mounting - handle both partition and file cases
    if disk_path.startswith("/dev/"):
        mount_ok = run_cmd(["sudo", "mount", disk_path, mount_point])
    else:
        run_cmd(["sudo", "modprobe", "loop"], check=False)
        mount_ok = run_cmd(["sudo", "mount", "-o", "loop", disk_path, mount_point])

    if not mount_ok:
        print("ERROR: Failed to mount disk")
        return None

    # Create a test file
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    test_filename = f"test_log_{timestamp}.txt"
    test_filepath = os.path.join(mount_point, test_filename)

    print(f"\nCreating test file: {test_filename}")
    test_content = f"""USB Airbridge Test Log
======================
Timestamp: {timestamp}
Test ID: E2E-{int(time.time())}
Device: Raspberry Pi Zero
Modem: SIM7000G

This is a test file to verify the complete pipeline:
1. USB gadget functionality
2. Local mount and file access
3. Zip compression
4. Cellular FTP upload

Lorem ipsum test data follows...
{'X' * 500}
"""

    with open(test_filepath, 'w') as f:
        f.write(test_content)
    os.sync()

    # List files on disk
    print("\nFiles on virtual disk:")
    for item in os.listdir(mount_point):
        filepath = os.path.join(mount_point, item)
        if os.path.isfile(filepath):
            size = os.path.getsize(filepath)
            print(f"  {item} ({size} bytes)")

    # Create zip archive
    zip_name = f"logs_{timestamp}.zip"
    zip_path = os.path.join(outbox_dir, zip_name)

    print(f"\nCreating archive: {zip_name}")
    files_added = 0
    with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
        for root, dirs, files in os.walk(mount_point):
            for file in files:
                if file.endswith(('.log', '.txt')):
                    full_path = os.path.join(root, file)
                    zipf.write(full_path, file)
                    print(f"  Added: {file}")
                    files_added += 1

    if files_added == 0:
        print("WARNING: No .log or .txt files found to archive")

    # Sync and unmount
    print("\nSyncing and unmounting...")
    os.sync()
    time.sleep(0.5)
    run_cmd(["sudo", "umount", mount_point])

    # Reload USB gadget
    print("\nReloading USB gadget...")
    run_cmd(["sudo", "modprobe", "g_mass_storage",
             f"file={disk_path}", "stall=0", "removable=1"])

    zip_size = os.path.getsize(zip_path)
    print(f"\nPhase 2 PASSED: Created {zip_name} ({zip_size} bytes)")
    return zip_path

def phase3_cellular_upload(zip_path):
    """Phase 3: Upload file via cellular FTP."""
    print("\n" + "="*50)
    print("PHASE 3: Cellular FTP Upload Test")
    print("="*50)

    ftp_cfg = cfg['ftp']

    print(f"\nTarget server: {ftp_cfg['server']}:{ftp_cfg['port']}")
    print(f"File to upload: {zip_path}")
    print(f"File size: {os.path.getsize(zip_path)} bytes")

    # Initialize modem handler
    print("\nInitializing SIM7000G modem...")
    modem = SIM7000GHandler(
        port=cfg['serial']['port'],
        baudrate=cfg['serial']['baudrate'],
        timeout=cfg['serial']['timeout'],
        chunk_size=cfg['chunk_size']
    )

    # Setup network
    print("\nSetting up cellular network...")
    ok, msg = modem.setup_network(apn=cfg['apn'])
    if not ok:
        print(f"ERROR: Network setup failed: {msg}")
        return False

    # Upload via FTP
    print("\nStarting FTP upload...")
    ok, msg = modem.upload_ftp(
        server=ftp_cfg['server'],
        port=ftp_cfg['port'],
        username=ftp_cfg['username'],
        password=ftp_cfg['password'],
        filepath=zip_path,
        remote_path=ftp_cfg['remote_path']
    )

    # Close bearer
    modem.close_bearer()

    if ok:
        print(f"\nPhase 3 PASSED: Upload successful!")
        # Clean up the uploaded file
        os.remove(zip_path)
        print(f"Removed local archive: {zip_path}")
        return True
    else:
        print(f"\nPhase 3 FAILED: {msg}")
        return False

def main():
    """Run complete end-to-end test."""
    print("="*50)
    print("USB CELLULAR AIRBRIDGE - END-TO-END TEST")
    print("="*50)
    print(f"Started at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"Virtual disk: {cfg['virtual_disk_path']}")
    print(f"FTP server: {cfg['ftp']['server']}")

    results = {}

    # Phase 1: USB Gadget
    results['phase1'] = phase1_usb_gadget()
    if not results['phase1']:
        print("\nABORTING: Phase 1 failed")
        return 1

    # Phase 2: Harvest
    zip_path = phase2_harvest()
    results['phase2'] = zip_path is not None
    if not results['phase2']:
        print("\nABORTING: Phase 2 failed")
        return 1

    # Phase 3: Upload
    results['phase3'] = phase3_cellular_upload(zip_path)

    # Summary
    print("\n" + "="*50)
    print("TEST SUMMARY")
    print("="*50)
    print(f"Phase 1 (USB Gadget):  {'PASS' if results['phase1'] else 'FAIL'}")
    print(f"Phase 2 (Harvesting):  {'PASS' if results['phase2'] else 'FAIL'}")
    print(f"Phase 3 (FTP Upload):  {'PASS' if results['phase3'] else 'FAIL'}")

    all_passed = all(results.values())
    print(f"\nOVERALL: {'ALL TESTS PASSED' if all_passed else 'SOME TESTS FAILED'}")
    print(f"Finished at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")

    return 0 if all_passed else 1

if __name__ == "__main__":
    sys.exit(main())

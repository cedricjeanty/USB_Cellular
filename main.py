#!/usr/bin/env python3
"""
USB Cellular Airbridge - Main Service
Monitors USB drive for new files, harvests them, uploads via cellular FTP.
"""

import os
import sys
import time
import yaml
import subprocess
import zipfile
import logging

# Setup logging for systemd journal
logging.basicConfig(
    level=logging.INFO,
    format='%(levelname)s: %(message)s',
    stream=sys.stdout
)
log = logging.getLogger('airbridge')

# Load configuration from absolute path
CONFIG_PATH = "/home/cedric/config.yaml"
with open(CONFIG_PATH, "r") as ymlfile:
    cfg = yaml.safe_load(ymlfile)

# Lazy-load modem to avoid serial issues at import time
modem = None

def get_modem():
    """Lazy initialization of modem handler."""
    global modem
    if modem is None:
        from modem_handler import SIM7000GHandler
        modem = SIM7000GHandler(
            port=cfg['serial']['port'],
            baudrate=cfg['serial']['baudrate'],
            timeout=cfg['serial']['timeout'],
            chunk_size=cfg['chunk_size']
        )
    return modem

def get_udc_state():
    """Get the current USB Device Controller state."""
    try:
        udc_dirs = os.listdir("/sys/class/udc/")
        if udc_dirs:
            with open(f"/sys/class/udc/{udc_dirs[0]}/state", 'r') as f:
                return f.read().strip()
    except Exception as e:
        log.warning(f"Could not read UDC state: {e}")
    return "unknown"

def is_gadget_loaded():
    """Check if USB gadget module is loaded."""
    result = subprocess.run(["lsmod"], capture_output=True, text=True)
    return "g_mass_storage" in result.stdout

def get_disk_write_sectors():
    """
    Get the cumulative write sectors from disk I/O stats.
    Returns the number of 512-byte sectors written, or -1 on error.
    """
    disk_path = cfg['virtual_disk_path']

    if not disk_path.startswith('/dev/'):
        return -1

    # Extract partition name (e.g., mmcblk0p3)
    part_name = disk_path.replace('/dev/', '')
    if 'mmcblk' in part_name:
        disk_name = part_name.rstrip('0123456789').rstrip('p')  # mmcblk0
    else:
        disk_name = part_name.rstrip('0123456789')  # sda

    stat_path = f"/sys/block/{disk_name}/{part_name}/stat"
    try:
        with open(stat_path, 'r') as f:
            stats = f.read().split()
            # Format: reads_completed reads_merged read_sectors read_ms
            #         writes_completed writes_merged write_sectors write_ms ...
            if len(stats) >= 7:
                return int(stats[6])  # write_sectors
    except Exception as e:
        log.warning(f"Could not read disk stats: {e}")

    return -1

def mount_usb_disk():
    """Mount the USB disk locally. Returns True on success."""
    disk_path = cfg['virtual_disk_path']
    mount_point = cfg['mount_point']

    os.makedirs(mount_point, exist_ok=True)
    subprocess.run(["umount", mount_point], capture_output=True)  # Cleanup any stale mount

    if disk_path.startswith("/dev/"):
        result = subprocess.run(["mount", disk_path, mount_point], capture_output=True)
    else:
        subprocess.run(["modprobe", "loop"], check=False)
        result = subprocess.run(["mount", "-o", "loop", disk_path, mount_point], capture_output=True)

    if result.returncode != 0:
        log.error(f"Failed to mount: {result.stderr.decode()}")
        return False
    return True

def unmount_usb_disk():
    """Unmount the USB disk."""
    mount_point = cfg['mount_point']
    os.sync()
    time.sleep(0.5)
    subprocess.run(["umount", mount_point], capture_output=True)

def harvest_and_upload():
    """
    Upload files from USB via cellular, delete only after confirmed upload.

    Key safety: Original files stay on USB until FTP confirms success.
    No zipping - files uploaded directly and remain visible to host if upload fails.
    """
    log.info("Starting upload cycle...")

    disk_path = cfg['virtual_disk_path']
    mount_point = cfg['mount_point']

    # 1. Unload USB Gadget
    log.info("Unloading USB gadget...")
    subprocess.run(["modprobe", "-r", "g_mass_storage"], check=False)
    time.sleep(1)

    # 2. Mount locally
    if not mount_usb_disk():
        log.error("Cannot mount disk, re-enabling gadget")
        load_usb_gadget()
        return False

    # 3. Scan for files to upload
    files_to_upload = []
    for root, dirs, files in os.walk(mount_point):
        # Skip hidden directories like .Trash
        dirs[:] = [d for d in dirs if not d.startswith('.')]
        for file in files:
            if file.startswith('.') or file in ('desktop.ini', 'Thumbs.db'):
                continue
            filepath = os.path.join(root, file)
            size = os.path.getsize(filepath)
            log.info(f"  Found: {file} ({size} bytes)")
            files_to_upload.append(filepath)

    if not files_to_upload:
        log.info("No files to upload")
        unmount_usb_disk()
        load_usb_gadget()
        return True

    # 4. Setup cellular network
    log.info(f"Uploading {len(files_to_upload)} file(s)...")
    ftp_cfg = cfg['ftp']
    m = get_modem()

    ok, msg = m.setup_network(apn=cfg['apn'])
    if not ok:
        log.error(f"Network setup failed: {msg}")
        log.warning("Files remain on USB, will retry next cycle")
        unmount_usb_disk()
        load_usb_gadget()
        return False

    # 5. Upload each file, delete only on success
    uploaded_count = 0
    failed_count = 0

    for filepath in files_to_upload:
        filename = os.path.basename(filepath)
        log.info(f"Uploading {filename}...")

        ok, msg = m.upload_ftp(
            server=ftp_cfg['server'],
            port=ftp_cfg['port'],
            username=ftp_cfg['username'],
            password=ftp_cfg['password'],
            filepath=filepath,
            remote_path=ftp_cfg['remote_path']
        )

        if ok:
            log.info(f"Upload successful, removing {filename}")
            os.remove(filepath)
            os.sync()
            uploaded_count += 1
        else:
            log.error(f"Upload FAILED for {filename}: {msg}")
            log.warning(f"  -> File remains on USB drive")
            failed_count += 1

    # 6. Close bearer
    m.close_bearer()

    # 7. Unmount and re-enable gadget
    if failed_count == 0:
        log.info(f"All {uploaded_count} file(s) uploaded successfully.")
    else:
        log.error(f"Upload complete: {uploaded_count} succeeded, {failed_count} FAILED")
        log.warning("Failed files remain visible on USB drive")

    log.info("Re-enabling USB gadget...")
    unmount_usb_disk()
    load_usb_gadget()

    return failed_count == 0

def upload_file(zip_path):
    """Upload a single file via cellular FTP. Used by boot phase."""
    ftp_cfg = cfg['ftp']

    log.info(f"Uploading {os.path.basename(zip_path)} to {ftp_cfg['server']}...")

    m = get_modem()

    # Setup network
    ok, msg = m.setup_network(apn=cfg['apn'])
    if not ok:
        log.error(f"Network setup failed: {msg}")
        return False

    # Upload via FTP
    ok, msg = m.upload_ftp(
        server=ftp_cfg['server'],
        port=ftp_cfg['port'],
        username=ftp_cfg['username'],
        password=ftp_cfg['password'],
        filepath=zip_path,
        remote_path=ftp_cfg['remote_path']
    )

    # Close bearer to save power
    m.close_bearer()

    if ok:
        log.info("Upload successful!")
        return True
    else:
        log.error(f"Upload failed: {msg}")
        return False

def load_usb_gadget():
    """Load the USB mass storage gadget."""
    disk_path = cfg['virtual_disk_path']
    log.info(f"Loading USB gadget with {disk_path}...")
    result = subprocess.run(
        ["modprobe", "g_mass_storage", f"file={disk_path}", "stall=0", "removable=1"],
        capture_output=True
    )
    if result.returncode == 0:
        log.info("USB gadget loaded successfully")
        return True
    else:
        log.error(f"Failed to load USB gadget: {result.stderr.decode()}")
        return False

def unload_usb_gadget():
    """Unload the USB mass storage gadget."""
    subprocess.run(["modprobe", "-r", "g_mass_storage"], capture_output=True)

def get_pending_uploads():
    """Get list of files waiting to be uploaded."""
    outbox = cfg['outbox_dir']
    if not os.path.exists(outbox):
        return []
    return [os.path.join(outbox, f) for f in os.listdir(outbox)
            if f.endswith(('.zip', '.gz'))]

def startup_upload_phase():
    """
    Boot-time phase: attempt to upload any pending files before enabling USB gadget.
    Outbox is on USB partition, so we need to mount disk to check/upload.
    Returns True if we should proceed to USB mode, False to retry uploads.
    """
    upload_timeout = cfg.get('boot_upload_timeout', 180)  # Default 3 minutes
    outbox_dir = cfg['outbox_dir']

    # Mount disk to check for pending uploads
    log.info("Mounting disk to check for pending uploads...")
    if not mount_usb_disk():
        log.error("Cannot mount disk during boot phase")
        return True  # Proceed to USB mode, can't check

    # Check for pending uploads
    pending = []
    if os.path.exists(outbox_dir):
        pending = [os.path.join(outbox_dir, f) for f in os.listdir(outbox_dir)
                   if f.endswith(('.zip', '.gz'))]

    if not pending:
        log.info("No pending uploads found")
        unmount_usb_disk()
        return True

    log.info(f"Found {len(pending)} pending upload(s)")
    for f in pending:
        log.info(f"  - {os.path.basename(f)} ({os.path.getsize(f)} bytes)")

    log.info(f"Attempting cellular upload (timeout: {upload_timeout}s)...")

    start_time = time.time()
    m = get_modem()

    # Try to establish network connection
    log.info("Connecting to cellular network...")
    ok, msg = m.setup_network(apn=cfg['apn'])

    if not ok:
        elapsed = time.time() - start_time
        if elapsed < upload_timeout:
            log.warning(f"Network setup failed: {msg}")
            log.info(f"Will retry... ({int(upload_timeout - elapsed)}s remaining)")
            # Keep disk mounted for retry
            return False
        else:
            log.warning(f"Network timeout after {upload_timeout}s. Proceeding to USB mode.")
            log.warning("Pending uploads will be retried next harvest cycle.")
            unmount_usb_disk()
            return True

    # Network connected - upload all pending files
    log.info("Network connected. Uploading pending files...")
    ftp_cfg = cfg['ftp']

    for filepath in pending:
        if time.time() - start_time > upload_timeout:
            log.warning("Upload timeout reached. Remaining files will wait.")
            break

        if not os.path.exists(filepath):
            continue

        log.info(f"Uploading {os.path.basename(filepath)}...")
        ok, msg = m.upload_ftp(
            server=ftp_cfg['server'],
            port=ftp_cfg['port'],
            username=ftp_cfg['username'],
            password=ftp_cfg['password'],
            filepath=filepath,
            remote_path=ftp_cfg['remote_path']
        )

        if ok:
            log.info(f"Upload successful. Removing {os.path.basename(filepath)}")
            os.remove(filepath)
            os.sync()
        else:
            log.error(f"Upload failed: {msg}")

    # Close bearer and unmount
    m.close_bearer()
    unmount_usb_disk()

    return True

def main():
    """Main loop for the Airbridge service."""
    log.info("=" * 40)
    log.info("USB Cellular Airbridge Starting")
    log.info("=" * 40)
    log.info(f"Virtual disk: {cfg['virtual_disk_path']}")
    log.info(f"FTP server: {cfg['ftp']['server']}")
    log.info(f"Quiet window: {cfg['quiet_window_seconds']}s")
    log.info(f"Poll interval: {cfg['poll_interval']}s")
    log.info(f"Outbox: {cfg['outbox_dir']} (on USB partition)")

    # Ensure USB gadget is not loaded during startup phase
    unload_usb_gadget()

    # === BOOT PHASE: Upload pending files first ===
    log.info("-" * 40)
    log.info("BOOT PHASE: Checking for pending uploads...")
    log.info("-" * 40)

    boot_timeout = cfg.get('boot_upload_timeout', 180)
    boot_start = time.time()

    while time.time() - boot_start < boot_timeout:
        if startup_upload_phase():
            break
        time.sleep(10)  # Retry network connection every 10s

    # === USB PHASE: Enable gadget and monitor ===
    log.info("-" * 40)
    log.info("USB PHASE: Enabling USB mass storage...")
    log.info("-" * 40)

    if not load_usb_gadget():
        log.error("Failed to load USB gadget. Exiting.")
        return 1

    # Track disk write activity
    baseline_sectors = get_disk_write_sectors()
    last_seen_sectors = baseline_sectors
    last_write_time = None  # None = no writes yet this session
    host_was_connected = False

    log.info("Monitoring for file writes...")
    log.info(f"(Will harvest {cfg['quiet_window_seconds']}s after last write activity)")

    while True:
        try:
            udc_state = get_udc_state()
            current_sectors = get_disk_write_sectors()

            # Check if gadget is loaded and host is connected
            if udc_state == "configured":
                if not host_was_connected:
                    log.info("Host connected. Waiting for file writes...")
                    host_was_connected = True
                    # Reset baseline when host connects
                    baseline_sectors = current_sectors
                    last_seen_sectors = current_sectors

                # Check for new write activity
                if current_sectors > last_seen_sectors:
                    bytes_written = (current_sectors - last_seen_sectors) * 512
                    total_written = (current_sectors - baseline_sectors) * 512
                    log.info(f"Write activity detected: +{bytes_written} bytes (total: {total_written} bytes)")
                    last_seen_sectors = current_sectors
                    last_write_time = time.time()

                # Check if we should harvest (had writes AND quiet period elapsed)
                if last_write_time is not None:
                    quiet_elapsed = time.time() - last_write_time
                    if quiet_elapsed >= cfg['quiet_window_seconds']:
                        total_written = (current_sectors - baseline_sectors) * 512
                        log.info(f"Quiet window elapsed. {total_written} bytes written this session.")
                        log.info("Starting harvest cycle...")

                        # Combined harvest + upload
                        harvest_and_upload()

                        # Reset tracking for next session
                        baseline_sectors = get_disk_write_sectors()
                        last_seen_sectors = baseline_sectors
                        last_write_time = None
                        host_was_connected = False  # Will re-log on next connection

            else:
                # Host not connected or gadget not loaded
                if host_was_connected:
                    log.info(f"Host disconnected (state: {udc_state})")
                    # If there were writes, harvest now
                    if last_write_time is not None:
                        total_written = (last_seen_sectors - baseline_sectors) * 512
                        log.info(f"Harvesting {total_written} bytes before disconnect...")
                        harvest_and_upload()
                    # Reset state
                    last_write_time = None
                host_was_connected = False

            time.sleep(cfg['poll_interval'])

        except KeyboardInterrupt:
            log.info("Shutting down...")
            break
        except Exception as e:
            log.error(f"Error in main loop: {e}")
            time.sleep(10)  # Back off on errors

    return 0

if __name__ == "__main__":
    sys.exit(main())
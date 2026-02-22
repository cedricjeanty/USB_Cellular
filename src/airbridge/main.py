#!/usr/bin/env python3
"""
USB Cellular Airbridge - Main Service
Monitors USB drive for new files, harvests them, uploads via cellular FTP.
"""

import os
import re
import sys
import time
import yaml
import subprocess
import zipfile
import logging

from display_handler import AirbridgeDisplay

# Setup logging for systemd journal
logging.basicConfig(
    level=logging.INFO,
    format='%(levelname)s: %(message)s',
    stream=sys.stdout
)
log = logging.getLogger('airbridge')

# Load configuration – try flat home-dir deployment first, then project tree
for _p in ("/home/cedric/config.yaml", "/home/cedric/USBCellular/config.yaml"):
    if os.path.exists(_p):
        CONFIG_PATH = _p
        break
else:
    raise FileNotFoundError("config.yaml not found in ~ or project tree")

with open(CONFIG_PATH, "r") as ymlfile:
    cfg = yaml.safe_load(ymlfile)

# ── Module-level singletons ────────────────────────────────────────────────────

modem   = None   # lazy-initialised on first use
display = None   # initialised in main()

# Current display state – updated throughout the state machine
_ds = dict(
    csq=99,
    carrier="",
    net_connected=False,
    usb_active=False,
    mb_uploaded=0.0,
    mb_remaining=0.0,
)


def _refresh_display():
    if display is not None:
        display.update(**_ds)


def _poll_modem_signal():
    """Update CSQ and carrier in _ds. Initialises modem serial port if needed."""
    try:
        m = get_modem()
        ok, resp = m.send_at('AT+CSQ', timeout=3)
        if ok:
            hit = re.search(r'\+CSQ: ?(\d+)', resp)
            if hit:
                _ds['csq'] = int(hit.group(1))
        ok, resp = m.send_at('AT+COPS?', timeout=3)
        if ok:
            hit = re.search(r'\+COPS: \d+,\d+,"([^"]+)"', resp)
            if hit:
                _ds['carrier'] = hit.group(1)
    except Exception as exc:
        log.debug(f"Signal poll failed: {exc}")


# ── Modem ─────────────────────────────────────────────────────────────────────

def get_modem():
    """Lazy initialization of modem handler."""
    global modem
    if modem is None:
        from modem_handler import SIM7000GHandler
        modem = SIM7000GHandler(
            port=cfg['modem']['port'],
            baudrate=cfg['modem']['baudrate'],
            timeout=cfg['modem']['timeout'],
            chunk_size=cfg['modem']['chunk_size'],
        )
    return modem


# ── USB / UDC helpers ─────────────────────────────────────────────────────────

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


def get_disk_write_sectors():
    """
    Get the cumulative write sectors from disk I/O stats.
    Returns the number of 512-byte sectors written, or -1 on error.
    """
    disk_path = cfg['virtual_disk_path']

    if not disk_path.startswith('/dev/'):
        return -1

    part_name = disk_path.replace('/dev/', '')
    if 'mmcblk' in part_name:
        disk_name = part_name.rstrip('0123456789').rstrip('p')
    else:
        disk_name = part_name.rstrip('0123456789')

    stat_path = f"/sys/block/{disk_name}/{part_name}/stat"
    try:
        with open(stat_path, 'r') as f:
            stats = f.read().split()
            if len(stats) >= 7:
                return int(stats[6])  # write_sectors
    except Exception as e:
        log.warning(f"Could not read disk stats: {e}")

    return -1


def mount_usb_disk():
    """Mount the USB disk locally. Returns True on success."""
    disk_path   = cfg['virtual_disk_path']
    mount_point = cfg['mount_point']

    os.makedirs(mount_point, exist_ok=True)
    subprocess.run(["umount", mount_point], capture_output=True)

    if disk_path.startswith("/dev/"):
        result = subprocess.run(["mount", disk_path, mount_point],
                                capture_output=True)
    else:
        subprocess.run(["modprobe", "loop"], check=False)
        result = subprocess.run(
            ["mount", "-o", "loop", disk_path, mount_point],
            capture_output=True)

    if result.returncode != 0:
        log.error(f"Failed to mount: {result.stderr.decode()}")
        return False
    return True


def unmount_usb_disk():
    """Unmount the USB disk."""
    os.sync()
    time.sleep(0.5)
    subprocess.run(["umount", cfg['mount_point']], capture_output=True)


def load_usb_gadget():
    """Load the USB mass storage gadget."""
    disk_path = cfg['virtual_disk_path']
    log.info(f"Loading USB gadget with {disk_path}...")
    result = subprocess.run(
        ["modprobe", "g_mass_storage",
         f"file={disk_path}", "stall=0", "removable=1"],
        capture_output=True,
    )
    if result.returncode == 0:
        log.info("USB gadget loaded successfully")
        _ds['usb_active'] = True
        _refresh_display()
        return True
    else:
        log.error(f"Failed to load USB gadget: {result.stderr.decode()}")
        return False


def unload_usb_gadget():
    """Unload the USB mass storage gadget."""
    subprocess.run(["modprobe", "-r", "g_mass_storage"], capture_output=True)
    _ds['usb_active'] = False


# ── Core phases ───────────────────────────────────────────────────────────────

def harvest_and_upload():
    """
    Upload files from USB via cellular, delete only after confirmed upload.

    Key safety: Original files stay on USB until FTP confirms success.
    """
    log.info("Starting upload cycle...")

    disk_path   = cfg['virtual_disk_path']
    mount_point = cfg['mount_point']

    # 1. Unload USB gadget
    log.info("Unloading USB gadget...")
    unload_usb_gadget()
    _refresh_display()
    time.sleep(1)

    # 2. Mount locally
    if not mount_usb_disk():
        log.error("Cannot mount disk, re-enabling gadget")
        load_usb_gadget()
        return False

    # 3. Scan for files to upload
    files_to_upload = []
    for root, dirs, files in os.walk(mount_point):
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

    # Track upload progress for the display
    total_bytes = sum(os.path.getsize(f) for f in files_to_upload)
    _ds['mb_remaining'] = total_bytes / 1e6
    _ds['mb_uploaded']  = 0.0
    _refresh_display()

    # 4. Setup cellular network
    log.info(f"Uploading {len(files_to_upload)} file(s)...")
    ftp_cfg = cfg['ftp']
    m = get_modem()

    ok, msg = m.setup_network(apn=cfg['modem']['apn'])
    if not ok:
        log.error(f"Network setup failed: {msg}")
        log.warning("Files remain on USB, will retry next cycle")
        unmount_usb_disk()
        load_usb_gadget()
        return False

    _ds['net_connected'] = True
    _poll_modem_signal()
    _refresh_display()

    # 5. Upload each file, delete only on success
    uploaded_count = 0
    failed_count   = 0

    for filepath in files_to_upload:
        filename  = os.path.basename(filepath)
        file_size = os.path.getsize(filepath)
        log.info(f"Uploading {filename}...")

        ok, msg = m.upload_ftp(
            server=ftp_cfg['server'],
            port=ftp_cfg['port'],
            username=ftp_cfg['username'],
            password=ftp_cfg['password'],
            filepath=filepath,
            remote_path=ftp_cfg['remote_path'],
        )

        if ok:
            log.info(f"Upload successful, removing {filename}")
            os.remove(filepath)
            os.sync()
            uploaded_count       += 1
            _ds['mb_uploaded']   += file_size / 1e6
            _ds['mb_remaining']  -= file_size / 1e6
            _ds['mb_remaining']   = max(0.0, _ds['mb_remaining'])
        else:
            log.error(f"Upload FAILED for {filename}: {msg}")
            log.warning("  -> File remains on USB drive")
            failed_count += 1

        _refresh_display()

    # 6. Close bearer
    m.close_bearer()
    _ds['net_connected'] = False

    # 7. Unmount and re-enable gadget
    if failed_count == 0:
        log.info(f"All {uploaded_count} file(s) uploaded successfully.")
    else:
        log.error(f"Upload complete: {uploaded_count} succeeded, {failed_count} FAILED")

    log.info("Re-enabling USB gadget...")
    unmount_usb_disk()
    load_usb_gadget()

    return failed_count == 0


def startup_upload_phase():
    """
    Boot-time phase: attempt to upload any pending files before enabling USB gadget.
    Returns True if we should proceed to USB mode, False to retry.
    """
    upload_timeout = cfg.get('boot_upload_timeout', 180)
    outbox_dir     = cfg['outbox_dir']

    log.info("Mounting disk to check for pending uploads...")
    if not mount_usb_disk():
        log.error("Cannot mount disk during boot phase")
        return True

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

    total_bytes = sum(os.path.getsize(f) for f in pending if os.path.exists(f))
    _ds['mb_remaining'] = total_bytes / 1e6
    _ds['mb_uploaded']  = 0.0
    _refresh_display()

    log.info(f"Attempting cellular upload (timeout: {upload_timeout}s)...")
    start_time = time.time()
    m = get_modem()

    log.info("Connecting to cellular network...")
    ok, msg = m.setup_network(apn=cfg['modem']['apn'])

    if not ok:
        elapsed = time.time() - start_time
        if elapsed < upload_timeout:
            log.warning(f"Network setup failed: {msg}")
            log.info(f"Will retry... ({int(upload_timeout - elapsed)}s remaining)")
            return False
        else:
            log.warning(f"Network timeout after {upload_timeout}s. Proceeding to USB mode.")
            unmount_usb_disk()
            return True

    _ds['net_connected'] = True
    _poll_modem_signal()
    _refresh_display()

    ftp_cfg = cfg['ftp']
    for filepath in pending:
        if time.time() - start_time > upload_timeout:
            log.warning("Upload timeout reached. Remaining files will wait.")
            break
        if not os.path.exists(filepath):
            continue

        file_size = os.path.getsize(filepath)
        log.info(f"Uploading {os.path.basename(filepath)}...")
        ok, msg = m.upload_ftp(
            server=ftp_cfg['server'],
            port=ftp_cfg['port'],
            username=ftp_cfg['username'],
            password=ftp_cfg['password'],
            filepath=filepath,
            remote_path=ftp_cfg['remote_path'],
        )

        if ok:
            log.info(f"Upload successful. Removing {os.path.basename(filepath)}")
            os.remove(filepath)
            os.sync()
            _ds['mb_uploaded']  += file_size / 1e6
            _ds['mb_remaining'] -= file_size / 1e6
            _ds['mb_remaining']  = max(0.0, _ds['mb_remaining'])
        else:
            log.error(f"Upload failed: {msg}")

        _refresh_display()

    m.close_bearer()
    _ds['net_connected'] = False
    unmount_usb_disk()
    return True


# ── Main loop ─────────────────────────────────────────────────────────────────

def main():
    global display

    log.info("=" * 40)
    log.info("USB Cellular Airbridge Starting")
    log.info("=" * 40)
    log.info(f"Virtual disk: {cfg['virtual_disk_path']}")
    log.info(f"FTP server: {cfg['ftp']['server']}")
    log.info(f"Quiet window: {cfg['quiet_window_seconds']}s")
    log.info(f"Poll interval: {cfg['poll_interval']}s")

    # Initialise display (silent if hardware not present)
    display = AirbridgeDisplay()
    display.show_message("Airbridge", "Starting...", "")

    # Ensure USB gadget is not loaded during startup
    unload_usb_gadget()

    # === BOOT PHASE: upload any pending files first ===
    log.info("-" * 40)
    log.info("BOOT PHASE: Checking for pending uploads...")
    log.info("-" * 40)
    display.show_message("Boot phase", "Checking", "outbox...")

    boot_timeout = cfg.get('boot_upload_timeout', 180)
    boot_start   = time.time()

    while time.time() - boot_start < boot_timeout:
        if startup_upload_phase():
            break
        time.sleep(10)

    _ds['net_connected'] = False
    _ds['mb_uploaded']   = 0.0
    _ds['mb_remaining']  = 0.0

    # === USB PHASE: enable gadget and monitor ===
    log.info("-" * 40)
    log.info("USB PHASE: Enabling USB mass storage...")
    log.info("-" * 40)

    if not load_usb_gadget():
        log.error("Failed to load USB gadget. Exiting.")
        display.show_message("ERROR", "Gadget load", "failed")
        return 1

    baseline_sectors   = get_disk_write_sectors()
    last_seen_sectors  = baseline_sectors
    last_write_time    = None
    host_was_connected = False

    log.info("Monitoring for file writes...")
    log.info(f"(Will harvest {cfg['quiet_window_seconds']}s after last write activity)")

    while True:
        try:
            udc_state      = get_udc_state()
            current_sectors = get_disk_write_sectors()

            if udc_state == "configured":
                if not host_was_connected:
                    log.info("Host connected. Waiting for file writes...")
                    host_was_connected = True
                    baseline_sectors   = current_sectors
                    last_seen_sectors  = current_sectors

                if current_sectors > last_seen_sectors:
                    bytes_written = (current_sectors - last_seen_sectors) * 512
                    total_written = (current_sectors - baseline_sectors) * 512
                    log.info(f"Write activity: +{bytes_written} bytes "
                             f"(session total: {total_written} bytes)")
                    last_seen_sectors = current_sectors
                    last_write_time   = time.time()

                if last_write_time is not None:
                    quiet_elapsed = time.time() - last_write_time
                    if quiet_elapsed >= cfg['quiet_window_seconds']:
                        total_written = (current_sectors - baseline_sectors) * 512
                        log.info(f"Quiet window elapsed. "
                                 f"{total_written} bytes written this session.")
                        log.info("Starting harvest cycle...")

                        harvest_and_upload()

                        # Reset tracking for next session
                        baseline_sectors   = get_disk_write_sectors()
                        last_seen_sectors  = baseline_sectors
                        last_write_time    = None
                        host_was_connected = False
                        _ds['mb_uploaded']  = 0.0
                        _ds['mb_remaining'] = 0.0

            else:
                if host_was_connected:
                    log.info(f"Host disconnected (state: {udc_state})")
                    if last_write_time is not None:
                        total_written = (last_seen_sectors - baseline_sectors) * 512
                        log.info(f"Harvesting {total_written} bytes on disconnect...")
                        harvest_and_upload()
                        _ds['mb_uploaded']  = 0.0
                        _ds['mb_remaining'] = 0.0
                    last_write_time = None
                host_was_connected = False

            # Update display every poll cycle
            _ds['usb_active'] = (udc_state == "configured")
            _poll_modem_signal()
            _refresh_display()

            time.sleep(cfg['poll_interval'])

        except KeyboardInterrupt:
            log.info("Shutting down...")
            display.show_message("Shutdown", "", "")
            break
        except Exception as e:
            log.error(f"Error in main loop: {e}")
            time.sleep(10)

    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
USB Cellular Airbridge - Main Service

USB gadget stays up continuously; harvesting (brief USB takedown) runs only when
the quiet-window expires, then the gadget comes straight back up.  A background
daemon thread handles all cellular uploads independently so USB access is never
blocked by modem operations.
"""

import os
import re
import sys
import time
import yaml
import shutil
import struct
import threading
import subprocess
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

# Prevents concurrent USB gadget/disk access (main thread harvest only).
_harvest_lock = threading.Lock()

# Prevents concurrent serial port access between the main thread signal poll
# and the upload worker's modem operations.  Signal poll uses non-blocking
# tryacquire — it simply skips the cycle if the upload worker holds the port.
_modem_lock = threading.Lock()

# Current display state – written by main thread and upload_worker (GIL-safe
# for individual key assignments).  Only the main thread calls _refresh_display.
_ds = dict(
    csq=99,
    carrier="",
    net_connected=False,
    usb_active=False,
    mb_uploaded=0.0,
    mb_remaining=0.0,
    drive_mb=0.0,       # estimated MB on virtual disk since last harvest
    drive_total_mb=0.0, # total capacity of virtual disk (0 = unknown)
)

# Filenames successfully uploaded this process lifetime — avoids FTP re-upload
# without needing a slow AT+FTPSIZE round-trip.  Cleared on service restart.
_uploaded_this_session: set = set()


def _refresh_display():
    if display is not None:
        display.update(**_ds)


def _poll_modem_signal():
    """Update CSQ and carrier in _ds. Skips silently if upload worker owns port."""
    if not _modem_lock.acquire(blocking=False):
        log.debug("Signal poll skipped — modem busy")
        return
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
        log.info(f"Signal: CSQ={_ds['csq']} carrier='{_ds['carrier']}'")
    except Exception as exc:
        log.warning(f"Signal poll failed: {exc}")
    finally:
        _modem_lock.release()


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


def get_fat32_disk_info():
    """
    Read FAT32 boot sector and FSInfo to determine used and total space.
    Safe while the USB gadget is active — read-only access to the raw device.

    Returns (used_mb, total_mb) as floats.
    Returns (-1.0, -1.0) if the read fails or FSInfo is stale/unavailable.
    """
    device = cfg['virtual_disk_path']
    try:
        with open(device, 'rb') as f:
            boot = f.read(512)
            if len(boot) < 512:
                return -1.0, -1.0

            bps  = struct.unpack_from('<H', boot, 11)[0]  # bytes per sector
            spc  = struct.unpack_from('<B', boot, 13)[0]  # sectors per cluster
            rsvd = struct.unpack_from('<H', boot, 14)[0]  # reserved sectors
            nfat = struct.unpack_from('<B', boot, 16)[0]  # number of FATs
            tot  = struct.unpack_from('<I', boot, 32)[0]  # total sectors
            fsz  = struct.unpack_from('<I', boot, 36)[0]  # FAT size in sectors
            fsi  = struct.unpack_from('<H', boot, 48)[0]  # FSInfo sector number

            if bps == 0 or spc == 0 or tot == 0:
                return -1.0, -1.0

            f.seek(fsi * bps)
            info = f.read(512)
            if len(info) < 512:
                return -1.0, -1.0

            # Validate FSInfo signatures
            if (struct.unpack_from('<I', info,   0)[0] != 0x41615252 or
                struct.unpack_from('<I', info, 484)[0] != 0x61417272):
                return -1.0, -1.0

            free_clusters = struct.unpack_from('<I', info, 488)[0]
            if free_clusters == 0xFFFFFFFF:
                return -1.0, -1.0  # host hasn't written FSInfo yet

            data_start     = rsvd + nfat * fsz
            total_clusters = (tot - data_start) // spc
            used_clusters  = max(0, total_clusters - free_clusters)
            used_mb        = used_clusters  * spc * bps / 1e6
            total_mb       = total_clusters * spc * bps / 1e6
            return used_mb, total_mb

    except Exception as exc:
        log.debug(f"FAT32 disk info read failed: {exc}")
        return -1.0, -1.0


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
        return True
    else:
        log.error(f"Failed to load USB gadget: {result.stderr.decode()}")
        return False


def unload_usb_gadget():
    """Unload the USB mass storage gadget."""
    subprocess.run(["modprobe", "-r", "g_mass_storage"], capture_output=True)
    _ds['usb_active'] = False


# ── Harvest phase (main thread only) ─────────────────────────────────────────

def harvest_to_outbox():
    """
    Briefly take down the USB gadget, mount the disk, move new files to
    the outbox, then restore the gadget.  The upload_worker will pick up
    outbox files independently.

    Safety: acquires _harvest_lock so no concurrent USB access occurs.
    """
    outbox_dir  = cfg['outbox_dir']
    mount_point = cfg['mount_point']

    os.makedirs(outbox_dir, exist_ok=True)

    with _harvest_lock:
        log.info("Harvest: unloading USB gadget...")
        unload_usb_gadget()
        _ds['usb_active'] = False
        time.sleep(1)

        if not mount_usb_disk():
            log.error("Harvest: cannot mount disk — restoring gadget")
            load_usb_gadget()
            return

        copied = 0
        skipped = 0
        for root, dirs, files in os.walk(mount_point):
            dirs[:] = [d for d in dirs if not d.startswith('.')]
            for fname in files:
                if fname.startswith('.') or fname in ('desktop.ini', 'Thumbs.db'):
                    continue
                src  = os.path.join(root, fname)
                dest = os.path.join(outbox_dir, fname)
                if os.path.exists(dest):
                    log.debug(f"Harvest: {fname} already pending, skipping")
                    skipped += 1
                    continue
                try:
                    shutil.copy2(src, dest)
                    log.info(f"Harvest: copied {fname} → outbox")
                    copied += 1
                except Exception as exc:
                    log.error(f"Harvest: failed to copy {fname}: {exc}")

        os.sync()
        unmount_usb_disk()
        log.info(f"Harvest complete: {copied} copied, {skipped} already pending")

        # Update outbox total for the display
        _ds['mb_remaining'] = _outbox_total_mb()

        load_usb_gadget()
        _ds['usb_active'] = True


# ── Upload worker (background daemon thread) ──────────────────────────────────

def _outbox_total_mb():
    """Return the total size of files in the outbox in MB."""
    outbox_dir = cfg['outbox_dir']
    if not os.path.isdir(outbox_dir):
        return 0.0
    total = sum(
        os.path.getsize(os.path.join(outbox_dir, f))
        for f in os.listdir(outbox_dir)
        if os.path.isfile(os.path.join(outbox_dir, f))
    )
    return total / 1e6


def upload_worker():
    """
    Daemon thread: monitors the outbox and uploads files via cellular FTP.
    Never touches the USB gadget or virtual disk.
    """
    outbox_dir = cfg['outbox_dir']
    ftp_cfg    = cfg['ftp']
    check_interval = cfg.get('upload_check_interval', 300)

    while True:
        try:
            os.makedirs(outbox_dir, exist_ok=True)
            pending = sorted(
                os.path.join(outbox_dir, f)
                for f in os.listdir(outbox_dir)
                if os.path.isfile(os.path.join(outbox_dir, f))
            )

            if pending:
                log.info(f"Upload worker: {len(pending)} file(s) to upload")
                # Update display immediately so the queue is visible during network setup
                _ds['mb_remaining'] = _outbox_total_mb()
                network_ok = False
                with _modem_lock:
                    m = get_modem()
                    ok, msg = m.setup_network(apn=cfg['modem']['apn'])
                    if not ok:
                        log.warning(f"Upload worker: network setup failed: {msg}. Retrying later.")
                    else:
                        network_ok = True
                        _ds['net_connected'] = True

                if not network_ok:
                    time.sleep(check_interval)
                    continue

                with _modem_lock:

                    # Initial signal read now that we own the modem
                    ok2, csq = m.check_signal()
                    if ok2:
                        _ds['csq'] = csq

                    for filepath in pending:
                        if not os.path.exists(filepath):
                            continue
                        fname     = os.path.basename(filepath)
                        file_size = os.path.getsize(filepath)

                        # Skip files already uploaded this session (avoids costly
                        # AT+FTPSIZE round-trip which can leave modem in bad state)
                        if fname in _uploaded_this_session:
                            log.info(f"Upload worker: {fname} already uploaded, removing from outbox")
                            os.remove(filepath)
                            os.sync()
                            _ds['mb_remaining'] = _outbox_total_mb()
                            continue

                        # Progress callback: update display as chunks land
                        _pre_upload_mb = _ds['mb_uploaded']
                        def _on_progress(sent, total, csq=None, _pre=_pre_upload_mb, _fsz=file_size):
                            _ds['mb_uploaded']  = _pre + sent / 1e6
                            # outbox still contains this file during upload; subtract
                            # bytes already sent to avoid double-counting
                            _ds['mb_remaining'] = max(0.0, _outbox_total_mb() - sent / 1e6)
                            if csq is not None:
                                _ds['csq'] = csq

                        log.info(f"Upload worker: uploading {fname}...")
                        ok, msg = m.upload_ftp(
                            server=ftp_cfg['server'],
                            port=ftp_cfg['port'],
                            username=ftp_cfg['username'],
                            password=ftp_cfg['password'],
                            filepath=filepath,
                            remote_path=ftp_cfg['remote_path'],
                            progress_callback=_on_progress,
                        )

                        if ok:
                            log.info(f"Upload worker: {fname} uploaded OK, removing from outbox")
                            _uploaded_this_session.add(fname)
                            os.remove(filepath)
                            os.sync()
                            _ds['mb_uploaded']  = _pre_upload_mb + file_size / 1e6
                            _ds['mb_remaining'] = _outbox_total_mb()
                        else:
                            log.error(f"Upload worker: {fname} FAILED: {msg}")

                    m.close_bearer()
                    _ds['net_connected'] = False

            else:
                log.debug("Upload worker: outbox empty, sleeping")

        except Exception as exc:
            log.error(f"Upload worker error: {exc}")
            _ds['net_connected'] = False

        time.sleep(check_interval)


# ── Main loop ─────────────────────────────────────────────────────────────────

def main():
    global display

    log.info("=" * 40)
    log.info("USB Cellular Airbridge Starting")
    log.info("=" * 40)
    log.info(f"Virtual disk: {cfg['virtual_disk_path']}")
    log.info(f"FTP server: {cfg['ftp']['server']}")
    log.info(f"Quiet window: {cfg['quiet_window_seconds']}s")

    # Initialise display (silent if hardware not present)
    display = AirbridgeDisplay()
    display.show_message("Airbridge", "Starting...", "")

    # Ensure USB gadget is not loaded while we set up
    unload_usb_gadget()

    # Start the upload worker as a daemon so it exits with the main process
    t = threading.Thread(target=upload_worker, name="upload-worker", daemon=True)
    t.start()
    log.info("Upload worker thread started")

    # Enable USB gadget
    if not load_usb_gadget():
        log.error("Failed to load USB gadget. Exiting.")
        display.show_message("ERROR", "Gadget load", "failed")
        return 1

    baseline_sectors   = get_disk_write_sectors()
    last_seen_sectors  = baseline_sectors
    last_write_time    = None
    host_was_connected = False
    _signal_tick       = 0   # poll modem signal every 6 cycles (30 s)

    log.info("Monitoring for file writes...")
    log.info(f"(Will harvest {cfg['quiet_window_seconds']}s after last write activity)")

    while True:
        try:
            udc_state       = get_udc_state()
            current_sectors = get_disk_write_sectors()

            # ── Signal poll (every 30 s, non-blocking relative to upload thread) ─
            _signal_tick += 1
            if _signal_tick >= 6:
                _signal_tick = 0
                _poll_modem_signal()

            # ── Live drive size (FAT32 used + total, falls back to sector delta) ─
            fat_used, fat_total = get_fat32_disk_info()
            if fat_used >= 0.0:
                _ds['drive_mb'] = fat_used
            elif current_sectors >= 0 and baseline_sectors >= 0:
                _ds['drive_mb'] = max(
                    0.0, (current_sectors - baseline_sectors) * 512 / 1e6)
            _ds['drive_total_mb'] = fat_total if fat_total >= 0.0 else 0.0

            if udc_state == "configured":
                if not host_was_connected:
                    log.info("Host connected — monitoring writes...")
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
                        log.info(f"Quiet window elapsed "
                                 f"({total_written} bytes written). Harvesting...")
                        harvest_to_outbox()
                        # Reset write tracking (drive size stays — files remain on disk)
                        baseline_sectors   = get_disk_write_sectors()
                        last_seen_sectors  = baseline_sectors
                        last_write_time    = None
                        host_was_connected = False

            else:
                # Host disconnected — harvest if there was pending write activity
                if host_was_connected and last_write_time is not None:
                    total_written = (last_seen_sectors - baseline_sectors) * 512
                    log.info(f"Host disconnected; harvesting {total_written} bytes...")
                    harvest_to_outbox()
                host_was_connected = False
                last_write_time    = None

            _ds['usb_active'] = (udc_state == "configured")
            _refresh_display()
            time.sleep(5)   # tight poll for responsive drive_mb updates

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

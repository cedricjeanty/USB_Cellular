import os
import time
import subprocess
import yaml

# Load configuration from shared config file
with open("config.yaml", "r") as ymlfile:
    cfg = yaml.safe_load(ymlfile)

DISK_IMAGE = cfg['virtual_disk_path']
MOUNT_POINT = cfg['mount_point']
QUIET_WINDOW = cfg.get('quiet_window_seconds', 30)

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
    return result.returncode == 0

def check_disk():
    """Check if the disk image/partition exists."""
    if os.path.exists(DISK_IMAGE):
        print(f"Using disk: {DISK_IMAGE}")
        # Check if it's a block device or file
        if os.path.isfile(DISK_IMAGE):
            print(f"  Type: File-backed image")
        else:
            print(f"  Type: Block device/partition")
        return True
    else:
        print(f"ERROR: {DISK_IMAGE} not found")
        return False

def ensure_mount_point():
    """Ensure mount point directory exists."""
    if not os.path.exists(MOUNT_POINT):
        run_cmd(["sudo", "mkdir", "-p", MOUNT_POINT])

def load_usb_gadget():
    """Load the USB mass storage gadget."""
    print("Loading USB mass storage gadget...")
    # Ensure loop driver is loaded
    run_cmd(["sudo", "modprobe", "loop"], check=False)
    # First unload if already loaded
    subprocess.run(["sudo", "modprobe", "-r", "g_mass_storage"],
                   capture_output=True)
    time.sleep(0.5)
    # Load with our disk image
    return run_cmd([
        "sudo", "modprobe", "g_mass_storage",
        f"file={DISK_IMAGE}", "stall=0", "removable=1"
    ])

def unload_usb_gadget():
    """Unload the USB mass storage gadget."""
    print("Unloading USB mass storage gadget...")
    return run_cmd(["sudo", "modprobe", "-r", "g_mass_storage"])

def mount_disk_image():
    """Mount the disk image locally."""
    print(f"Mounting {DISK_IMAGE} to {MOUNT_POINT}...")
    if os.path.isfile(DISK_IMAGE):
        # File-backed image needs loop device
        run_cmd(["sudo", "modprobe", "loop"])
        return run_cmd(["sudo", "mount", "-o", "loop", DISK_IMAGE, MOUNT_POINT])
    else:
        # Block device/partition - mount directly
        return run_cmd(["sudo", "mount", DISK_IMAGE, MOUNT_POINT])

def unmount_disk_image():
    """Unmount the disk image."""
    print(f"Unmounting {MOUNT_POINT}...")
    run_cmd(["sudo", "sync"])
    time.sleep(1)  # Allow file handles to close
    return run_cmd(["sudo", "umount", MOUNT_POINT])

def get_udc_state():
    """Get the USB device controller state."""
    try:
        udc_path = "/sys/class/udc"
        udcs = os.listdir(udc_path)
        if udcs:
            state_file = os.path.join(udc_path, udcs[0], "state")
            with open(state_file, "r") as f:
                return f.read().strip()
    except Exception as e:
        print(f"Error reading UDC state: {e}")
    return "unknown"

def get_disk_activity():
    """Get disk write activity counter. Works for both files and block devices."""
    if os.path.isfile(DISK_IMAGE):
        # For file-backed images, use mtime
        try:
            return os.path.getmtime(DISK_IMAGE)
        except OSError:
            return 0
    else:
        # For block devices, read write sectors from /proc/diskstats
        # Format: major minor name reads ... writes ...
        # Field 10 (index 9) is sectors written
        device_name = os.path.basename(DISK_IMAGE)  # e.g., "mmcblk0p3"
        try:
            with open("/proc/diskstats", "r") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) >= 10 and parts[2] == device_name:
                        return int(parts[9])  # sectors written
        except Exception:
            pass
        return 0

def list_files():
    """List all files in the mounted directory and return their paths."""
    print(f"\n=== Files in {MOUNT_POINT} ===")
    file_list = []
    for root, dirs, files in os.walk(MOUNT_POINT):
        # Get relative path from mount point
        rel_root = os.path.relpath(root, MOUNT_POINT)
        if rel_root == ".":
            rel_root = ""

        for f in files:
            filepath = os.path.join(root, f)
            size = os.path.getsize(filepath)
            rel_path = os.path.join(rel_root, f) if rel_root else f
            print(f"  {rel_path} ({size} bytes)")
            file_list.append(filepath)

    if not file_list:
        print("  (empty)")
    print("=" * 40)
    return file_list


def monitor_transfers():
    """Monitor for file transfers and detect quiet periods."""
    print(f"\nMonitoring for file transfers (quiet window: {QUIET_WINDOW}s)...")
    print("Connect the Pi to a host via USB and copy some files.")
    print("Press Ctrl+C to stop monitoring.\n")

    last_activity = get_disk_activity()
    last_change_time = time.time()
    transfer_detected = False

    while True:
        current_activity = get_disk_activity()
        udc_state = get_udc_state()

        if current_activity != last_activity:
            if not transfer_detected:
                print(f"[{time.strftime('%H:%M:%S')}] Transfer activity detected!")
                transfer_detected = True
            last_activity = current_activity
            last_change_time = time.time()

        quiet_time = time.time() - last_change_time

        # Show status periodically
        if int(quiet_time) % 5 == 0 and int(quiet_time) > 0:
            print(f"[{time.strftime('%H:%M:%S')}] UDC: {udc_state}, Quiet for: {int(quiet_time)}s", end='\r')

        # Check if we've been quiet long enough after a transfer
        if transfer_detected and quiet_time >= QUIET_WINDOW:
            print(f"\n[{time.strftime('%H:%M:%S')}] Quiet window reached ({QUIET_WINDOW}s)")
            return True

        time.sleep(1)

def main():
    print("=== USB Mass Storage Test ===\n")

    # Setup
    if not check_disk():
        return
    ensure_mount_point()

    # Load USB gadget
    if not load_usb_gadget():
        print("Failed to load USB gadget")
        return

    print(f"\nUSB gadget loaded. UDC state: {get_udc_state()}")

    try:
        # Monitor for transfers
        if monitor_transfers():
            # Transfer complete, unmount and read files
            print("\nHarvesting files...")

            if not unload_usb_gadget():
                print("Failed to unload USB gadget")
                return

            time.sleep(1)

            if not mount_disk_image():
                print("Failed to mount disk image")
                return

            # List files
            list_files()

            unmount_disk_image()

            # Reload gadget for next round
            print("\nReloading USB gadget for next transfer...")
            load_usb_gadget()

    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    finally:
        print("\nCleaning up...")
        # Try to unmount if mounted
        subprocess.run(["sudo", "umount", MOUNT_POINT], capture_output=True)

if __name__ == "__main__":
    main()

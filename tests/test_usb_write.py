"""
USB write test – runs from the HOST MACHINE.
The Pi must be connected via USB cable and visible as a USB mass-storage drive.
WiFi (pizerologs.local) is used for SSH side-channel checks.

Steps:
  1. Confirm the Pi's USB gadget is loaded
  2. Mount the USB block device on this host
  3. Write a unique test file, sync, unmount
  4. Via SSH, verify the Pi's write-sector counter increased

Run:
    pytest tests/test_usb_write.py --pi-host cedric@pizerologs.local \\
           --usb-device /dev/sdb
"""

import os
import subprocess
import tempfile
import time
import uuid
from datetime import datetime

import pytest

MOUNT_TMP    = "/tmp/airbridge_usb_test"
SECTOR_WAIT  = 15   # seconds to wait for Pi to register write activity


# ── Helpers ───────────────────────────────────────────────────────────────────

def _local(cmd: str, check: bool = False) -> subprocess.CompletedProcess:
    """Run a local shell command; never raises by default."""
    return subprocess.run(cmd, shell=True, capture_output=True, text=True,
                          check=check)


def _write_sector_count(ssh) -> int:
    """Read the Pi's write-sector counter for mmcblk0p3 (falls back to 0)."""
    r = ssh.run(
        "awk '/mmcblk0p3/{print $10}' /proc/diskstats 2>/dev/null "
        "|| stat -c %Y /dev/mmcblk0p3 2>/dev/null || echo 0"
    )
    try:
        return int(r.stdout.strip() or "0")
    except ValueError:
        return 0


# ── Tests ─────────────────────────────────────────────────────────────────────

@pytest.mark.hardware
@pytest.mark.usb_cable
def test_gadget_loaded_on_pi(ssh):
    """g_mass_storage module is loaded on the Pi while USB cable is attached."""
    r = ssh.run("lsmod | grep g_mass_storage")
    assert "g_mass_storage" in r.stdout, (
        "g_mass_storage is not loaded on the Pi.\n"
        "Run: sudo modprobe g_mass_storage file=/dev/mmcblk0p3 stall=0 removable=1"
    )


@pytest.mark.hardware
@pytest.mark.usb_cable
def test_udc_configured(ssh):
    """UDC reports 'configured' – host OS has enumerated the drive."""
    r = ssh.run("cat /sys/class/udc/*/state 2>/dev/null || echo unknown")
    state = r.stdout.strip()
    # "not attached" is allowed in case the host hasn't enumerated yet;
    # "configured" means fully enumerated.
    assert state in {"configured", "not attached", "suspended"}, (
        f"Unexpected UDC state: {state!r} – check USB cable and gadget"
    )


@pytest.mark.hardware
@pytest.mark.usb_cable
@pytest.mark.disruptive
def test_write_file_and_verify(ssh, usb_device):
    """
    Mount the USB device on this host, write a test file, sync+unmount,
    then verify that the Pi's diskstat write counter increased.
    """
    os.makedirs(MOUNT_TMP, exist_ok=True)
    _local(f"sudo umount {MOUNT_TMP}")   # clear any stale mount

    # Try partition-1 first, then bare device
    mounted_as = None
    for target in [f"{usb_device}1", usb_device]:
        r = _local(f"sudo mount {target} {MOUNT_TMP}")
        if r.returncode == 0:
            mounted_as = target
            break

    assert mounted_as is not None, (
        f"Could not mount {usb_device} or {usb_device}1 at {MOUNT_TMP}.\n"
        f"Ensure the USB cable is data-capable and the gadget is loaded on the Pi."
    )

    try:
        uid      = uuid.uuid4().hex[:8]
        filename = (f"airbridge_test_"
                    f"{datetime.now().strftime('%Y%m%d_%H%M%S')}_{uid}.txt")
        path     = os.path.join(MOUNT_TMP, filename)
        content  = (
            f"USB Airbridge write test\n"
            f"Host:      {os.uname().nodename}\n"
            f"File:      {filename}\n"
            f"Timestamp: {datetime.now().isoformat()}\n"
            f"UID:       {uid}\n"
        )
        with open(path, "w") as f:
            f.write(content)

        _local("sync", check=True)
        time.sleep(0.5)
    finally:
        _local(f"sudo umount {MOUNT_TMP}")

    # Poll Pi's write-sector counter
    baseline = _write_sector_count(ssh)
    deadline = time.time() + SECTOR_WAIT
    detected = False
    while time.time() < deadline:
        time.sleep(1)
        current = _write_sector_count(ssh)
        if current != baseline:
            detected = True
            break

    assert detected, (
        f"Pi did not register any write activity on mmcblk0p3 within {SECTOR_WAIT} s.\n"
        "Check: USB cable has data lines, gadget still loaded, host flushed writes."
    )

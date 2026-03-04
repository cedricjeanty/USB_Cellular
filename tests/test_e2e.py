"""
End-to-end tests – SSH to the Pi and exercise the full Airbridge pipeline.
Replaces: e2e_test.py

Phases:
  Phase 1  (disruptive)  USB gadget loads, UDC enters a valid state
  Phase 2  (disruptive)  Gadget unloads, disk mounts, files archived, disk unmounts,
                         gadget reloads
  Phase 3  (cellular)    Archive uploaded via FTP over WiFi, then deleted from outbox  [--cellular]

Run:
    # Phase 1 + 2 only (no upload):
    pytest tests/test_e2e.py --pi-host cedric@pizerologs.local --disruptive

    # Full pipeline (WiFi upload):
    pytest tests/test_e2e.py --pi-host cedric@pizerologs.local --disruptive --cellular
"""

import pytest


# ── Helpers ───────────────────────────────────────────────────────────────────

def _pi(ssh, code: str, timeout: int = 30) -> str:
    """
    Run a Python script on the Pi via stdin.
    Returns stdout; raises AssertionError on non-zero exit.
    """
    r = ssh.script(code, timeout=timeout)
    assert r.returncode == 0, (
        f"Pi snippet failed:\n  {code!r}\n"
        f"  stdout: {r.stdout.strip()}\n  stderr: {r.stderr.strip()}"
    )
    return r.stdout


# ── Phase 1: USB Gadget ───────────────────────────────────────────────────────

@pytest.mark.hardware
@pytest.mark.disruptive
def test_e2e_gadget_loads(ssh, pi_config):
    """Phase 1a – g_mass_storage loads without error."""
    disk = pi_config["virtual_disk_path"]
    r = ssh.run("sudo modprobe -r g_mass_storage")   # unload first (best-effort)
    r = ssh.run(
        f"sudo modprobe g_mass_storage file={disk} stall=0 removable=1"
    )
    assert r.returncode == 0, (
        f"modprobe g_mass_storage failed:\n{r.stderr.strip()}"
    )


@pytest.mark.hardware
@pytest.mark.disruptive
def test_e2e_udc_valid_after_gadget_load(ssh):
    """Phase 1b – UDC enters a valid state after gadget load."""
    r = ssh.check("cat /sys/class/udc/*/state")
    state = r.stdout.strip()
    assert state in {"configured", "not attached", "suspended", "powered"}, (
        f"Unexpected UDC state: {state!r}"
    )


# ── Phase 2: Harvest ──────────────────────────────────────────────────────────

@pytest.mark.hardware
@pytest.mark.disruptive
def test_e2e_harvest_creates_archive(ssh, pi_config):
    """
    Phase 2 – Unload gadget, mount disk, create a test file, zip it into
    the outbox, unmount, reload gadget.  Verifies an archive exists in the
    outbox afterwards.
    """
    disk     = pi_config["virtual_disk_path"]
    mp       = pi_config["mount_point"]
    outbox   = pi_config["outbox_dir"]

    # Unload gadget
    ssh.run("sudo modprobe -r g_mass_storage")

    # Ensure mount point and outbox exist
    ssh.check(f"mkdir -p {mp} {outbox}")

    # Unmount if already mounted
    ssh.run(f"sudo umount {mp}")

    # Mount disk
    if disk.startswith("/dev/"):
        mount_cmd = f"sudo mount {disk} {mp}"
    else:
        mount_cmd = f"sudo mount -o loop {disk} {mp}"
    r = ssh.run(mount_cmd)
    assert r.returncode == 0, f"mount failed: {r.stderr.strip()}"

    try:
        # Create a small test file on the virtual disk
        ssh.check(
            f"echo 'e2e test $(date)' | sudo tee {mp}/e2e_test_$(date +%s).txt > /dev/null"
        )

        # Ensure outbox is writable by the cedric user
        ssh.check(f"sudo chown -R cedric: {outbox}")

        # Archive .txt/.log files into outbox
        code = (
            "import os, zipfile, time\n"
            f"mp = {mp!r}\n"
            f"outbox = {outbox!r}\n"
            "zname = f'logs_e2e_{int(time.time())}.zip'\n"
            "zpath = os.path.join(outbox, zname)\n"
            "count = 0\n"
            "with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as zf:\n"
            "    for f in os.listdir(mp):\n"
            "        if f.endswith(('.txt', '.log')):\n"
            "            zf.write(os.path.join(mp, f), f)\n"
            "            count += 1\n"
            "os.sync()\n"
            "print(zname, count)\n"
        )
        out = _pi(ssh, code, timeout=30)
        zname, count_str = out.strip().split()
        assert int(count_str) > 0, "No .txt/.log files found to archive"
    finally:
        # Always unmount and reload gadget
        ssh.run(f"sudo umount {mp}")
        ssh.run(f"sudo modprobe g_mass_storage file={disk} stall=0 removable=1")

    # Verify the archive is in the outbox and is a valid zip
    r = ssh.check(f"python3 -c \"import zipfile; zf = zipfile.ZipFile('{outbox}/{zname}'); print(len(zf.namelist()))\"")
    assert int(r.stdout.strip()) > 0, "Archive in outbox is empty"


# ── Phase 3: WiFi Upload ──────────────────────────────────────────────────────

@pytest.mark.hardware
@pytest.mark.cellular
@pytest.mark.slow
def test_e2e_wifi_upload(ssh, pi_config):
    """
    Phase 3 – Pick the oldest archive from the outbox, upload it via
    FTP over WiFi, confirm success, delete the local file.
    Requires WiFi connectivity and a configured FTP server.
    """
    ftp    = pi_config["ftp"]
    outbox = pi_config["outbox_dir"]

    # Find any .zip in the outbox (may have been created by Phase 2 or earlier)
    r = ssh.check(f"ls {outbox}/*.zip 2>/dev/null | head -1")
    zip_path = r.stdout.strip()
    assert zip_path, (
        f"No .zip files found in {outbox} – run Phase 2 first or place a file manually"
    )

    code = (
        "import sys\n"
        "sys.path.insert(0, '/home/cedric')\n"
        "from wifi_manager import is_connected, upload_ftp\n"
        "import yaml\n"
        "cfg = yaml.safe_load(open('/home/cedric/config.yaml'))\n"
        "ftp = cfg['ftp']\n"
        f"assert is_connected(), 'No WiFi — cannot upload'\n"
        f"ok, msg = upload_ftp(\n"
        f"    server=ftp['server'], port=ftp['port'],\n"
        f"    username=ftp['username'], password=ftp['password'],\n"
        f"    filepath={zip_path!r}, remote_path=ftp['remote_path'])\n"
        f"print('uploaded' if ok else f'FAILED: {{msg}}')\n"
    )
    r = ssh.script(code, timeout=120)
    assert r.returncode == 0 and "uploaded" in r.stdout, (
        f"WiFi FTP upload failed:\n  stdout: {r.stdout.strip()}\n  stderr: {r.stderr.strip()}"
    )

    # Delete the uploaded file from the outbox
    ssh.run(f"rm -f {zip_path}")

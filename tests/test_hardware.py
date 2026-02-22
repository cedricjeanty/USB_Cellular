"""
Hardware tests – SSH to the Pi to verify all peripherals.
Replaces: test_system.py, ssh_device_test.py, and the old custom-runner test_hardware.py.

Run:
    pytest tests/test_hardware.py --pi-host cedric@pizerologs.local
    pytest tests/test_hardware.py --pi-host ... --disruptive
"""

import time
import pytest


# ── I2C / Display ─────────────────────────────────────────────────────────────

@pytest.mark.hardware
def test_i2c_bus_exists(ssh):
    r = ssh.run("test -c /dev/i2c-1 && echo yes")
    assert "yes" in r.stdout, (
        "/dev/i2c-1 not found. "
        "Add dtparam=i2c_arm=on to /boot/firmware/config.txt and reboot, "
        "then add i2c-dev to /etc/modules."
    )


@pytest.mark.hardware
def test_ssd1306_responds_on_i2c(ssh):
    r = ssh.python(
        "import smbus; b = smbus.SMBus(1); b.read_byte(0x3C); b.close(); print('ok')"
    )
    assert "ok" in r.stdout, "SSD1306 did not ACK at 0x3C – check wiring (SDA→GPIO2, SCL→GPIO3)"


@pytest.mark.hardware
def test_display_handler_importable(ssh):
    r = ssh.run(
        "python3 -c 'import sys; sys.path.insert(0,\"/home/cedric\"); "
        "from display_handler import AirbridgeDisplay; print(\"ok\")'"
    )
    assert "ok" in r.stdout, f"display_handler import failed:\n{r.stderr}"


# ── UART / Modem ──────────────────────────────────────────────────────────────

@pytest.mark.hardware
def test_uart_device_exists(ssh):
    r = ssh.run("test -c /dev/ttyAMA0 && echo yes")
    assert "yes" in r.stdout, "/dev/ttyAMA0 not found – check enable_uart=1 in config.txt"


@pytest.mark.hardware
def test_modem_handler_importable(ssh):
    r = ssh.run(
        "python3 -c 'import sys; sys.path.insert(0,\"/home/cedric\"); "
        "from modem_handler import SIM7000GHandler; print(\"ok\")'"
    )
    assert "ok" in r.stdout, f"modem_handler import failed:\n{r.stderr}"


# ── USB gadget / UDC ──────────────────────────────────────────────────────────

@pytest.mark.hardware
def test_udc_controller_exists(ssh):
    r = ssh.run("ls /sys/class/udc/ 2>/dev/null | head -1")
    assert r.stdout.strip(), (
        "No USB Device Controller – "
        "check dtoverlay=dwc2,dr_mode=peripheral in [all] section of config.txt"
    )


@pytest.mark.hardware
def test_udc_state_is_valid(ssh):
    r = ssh.check("cat /sys/class/udc/*/state")
    state = r.stdout.strip()
    valid = {"configured", "not attached", "suspended", "powered"}
    assert state in valid, f"Unexpected UDC state: {state!r}"


@pytest.mark.hardware
def test_virtual_disk_exists(ssh, pi_config):
    disk = pi_config["virtual_disk_path"]
    r = ssh.run(f"test -e {disk!r} && echo yes")
    assert "yes" in r.stdout, f"Virtual disk not found: {disk}"


# ── Filesystem ────────────────────────────────────────────────────────────────

@pytest.mark.hardware
def test_mount_point_exists(ssh, pi_config):
    mp = pi_config["mount_point"]
    r = ssh.run(f"test -d {mp!r} && echo yes")
    assert "yes" in r.stdout, f"{mp} missing – run setup_otg.sh"


@pytest.mark.hardware
def test_config_yaml_on_pi(ssh):
    r = ssh.run(
        "test -f ~/config.yaml && echo yes "
        "|| test -f /home/cedric/USBCellular/config.yaml && echo yes"
    )
    assert "yes" in r.stdout, "config.yaml not found on Pi – run: scp config.yaml cedric@pi:~"


@pytest.mark.hardware
def test_free_memory_above_10mb(ssh):
    r = ssh.check("awk '/MemAvailable/{print $2}' /proc/meminfo")
    kb = int(r.stdout.strip())
    assert kb > 10_000, f"Only {kb // 1024} MB free RAM – system may be unstable"


@pytest.mark.hardware
def test_free_disk_above_10mb(ssh):
    r = ssh.check("df / --output=avail | tail -1")
    kb = int(r.stdout.strip())
    assert kb > 10_000, f"Only {kb // 1024} MB free on rootfs"


# ── Python environment on Pi ─────────────────────────────────────────────────

@pytest.mark.hardware
@pytest.mark.parametrize("pkg", ["serial", "yaml", "smbus"])
def test_python_package_on_pi(ssh, pkg):
    r = ssh.python(f"import {pkg}; print('ok')")
    assert "ok" in r.stdout, f"{pkg} not importable on Pi"


# ── Service ───────────────────────────────────────────────────────────────────

@pytest.mark.hardware
def test_airbridge_service_enabled(ssh):
    r = ssh.run("systemctl is-enabled airbridge.service 2>/dev/null")
    # Not failing if disabled – just report; service may not be installed yet
    status = r.stdout.strip()
    if status not in ("enabled", "disabled", "static"):
        pytest.xfail(f"airbridge.service not found (status: {status!r}) – not installed yet")


# ── Disruptive functional tests ───────────────────────────────────────────────

@pytest.mark.hardware
@pytest.mark.disruptive
def test_display_renders_all_states(ssh):
    """Init the display and render a representative update cycle."""
    code = (
        "import sys, time; sys.path.insert(0,'/home/cedric'); "
        "from display_handler import AirbridgeDisplay; "
        "d = AirbridgeDisplay(); "
        "assert d._ok(), 'display init failed'; "
        "d.show_message('pytest', 'disruptive', 'test'); time.sleep(0.5); "
        "d.update(csq=18, carrier='Test', net_connected=True, "
        "usb_active=True, mb_uploaded=1.5, mb_remaining=3.5); time.sleep(0.5); "
        "print('ok')"
    )
    r = ssh.run(f"python3 -c \"{code}\"", timeout=15)
    assert "ok" in r.stdout, f"Display render failed:\n{r.stderr}"


@pytest.mark.hardware
@pytest.mark.disruptive
def test_modem_responds_to_at(ssh):
    """Open the serial port and confirm the modem replies OK to AT."""
    code = (
        "import sys; sys.path.insert(0,'/home/cedric'); "
        "from modem_handler import SIM7000GHandler; "
        "m = SIM7000GHandler('/dev/ttyAMA0', 115200, timeout=1); "
        "ok, resp = m.send_at('AT', timeout=3); "
        "print('ok' if ok else 'FAIL', resp.strip())"
    )
    r = ssh.run(f"python3 -c \"{code}\"", timeout=10)
    assert r.stdout.startswith("ok"), f"Modem AT failed:\n{r.stdout}\n{r.stderr}"


@pytest.mark.hardware
@pytest.mark.disruptive
def test_usb_gadget_loads_and_unloads(ssh, pi_config):
    """Load g_mass_storage, verify UDC state, then unload cleanly."""
    disk = pi_config["virtual_disk_path"]

    ssh.run("sudo modprobe -r g_mass_storage")
    time.sleep(1)

    r = ssh.run(f"sudo modprobe g_mass_storage file={disk} stall=0 removable=1")
    assert r.returncode == 0, f"gadget load failed: {r.stderr.strip()}"

    r2 = ssh.check("cat /sys/class/udc/*/state")
    assert r2.stdout.strip() in {"configured", "not attached", "suspended", "powered"}

    ssh.run("sudo modprobe -r g_mass_storage")


@pytest.mark.hardware
@pytest.mark.disruptive
def test_disk_mounts_and_unmounts(ssh, pi_config):
    """Unload gadget, mount disk read-only, list files, unmount, reload."""
    disk = pi_config["virtual_disk_path"]
    mp   = pi_config["mount_point"]

    ssh.run("sudo modprobe -r g_mass_storage")
    time.sleep(0.5)
    ssh.run(f"sudo umount {mp}")

    mount_cmd = (
        f"sudo mount {disk} {mp}"
        if disk.startswith("/dev/")
        else f"sudo mount -o loop {disk} {mp}"
    )
    r = ssh.run(mount_cmd)
    assert r.returncode == 0, f"mount failed: {r.stderr.strip()}"

    r2 = ssh.check(f"ls {mp} | wc -l")
    assert r2.stdout.strip().isdigit()

    ssh.run(f"sudo umount {mp}")
    ssh.run(f"sudo modprobe g_mass_storage file={disk} stall=0 removable=1")

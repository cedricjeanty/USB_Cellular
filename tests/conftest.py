"""
conftest.py – shared fixtures and auto-skip logic for the Airbridge test suite.

Mark summary
────────────
  hardware    SSH to the Pi to test peripherals.   Enable: --pi-host USER@HOST
  cellular    Requires WiFi network connection.     Enable: --cellular
  disruptive  Briefly unloads the USB gadget.       Enable: --disruptive
  usb_cable   Pi connected via USB to this host.    Enable: --usb-device /dev/sdX
  slow        Takes > 30 s.

Example runs
────────────
  pytest                                         # unit tests only
  pytest --pi-host cedric@pizerologs.local       # + all hardware tests
  pytest --pi-host ... --disruptive              # + gadget/mount tests
  pytest --pi-host ... --cellular                # + WiFi upload tests
  pytest --pi-host ... --usb-device /dev/sdb     # + USB-cable write test
"""

import subprocess
import pytest
import yaml

DEFAULT_HOST = "cedric@pizerologs.local"
_SSH_OPTS    = ["-o", "ConnectTimeout=10",
                "-o", "BatchMode=yes",
                "-o", "StrictHostKeyChecking=no"]


# ── CLI options ───────────────────────────────────────────────────────────────

def pytest_addoption(parser):
    parser.addoption(
        "--pi-host", default=None, metavar="USER@HOST",
        help=f"SSH target for Pi (default: {DEFAULT_HOST})",
    )
    parser.addoption(
        "--cellular", action="store_true", default=False,
        help="Enable WiFi upload tests (requires network connection)",
    )
    parser.addoption(
        "--disruptive", action="store_true", default=False,
        help="Enable tests that briefly unload the USB gadget",
    )
    parser.addoption(
        "--usb-device", default=None, metavar="DEV",
        help="USB block device on this host for USB-cable tests (e.g. /dev/sdb)",
    )


# ── Auto-skip wiring ──────────────────────────────────────────────────────────

def pytest_collection_modifyitems(config, items):
    skip = {
        "hardware":   pytest.mark.skip(reason="pass --pi-host USER@HOST to enable"),
        "cellular":   pytest.mark.skip(reason="pass --cellular to enable"),
        "disruptive": pytest.mark.skip(reason="pass --disruptive to enable"),
        "usb_cable":  pytest.mark.skip(reason="pass --usb-device /dev/sdX to enable"),
    }
    pi_host    = config.getoption("--pi-host")
    cellular   = config.getoption("--cellular")
    disruptive = config.getoption("--disruptive")
    usb_device = config.getoption("--usb-device")

    for item in items:
        marks = {m.name for m in item.iter_markers()}
        if "hardware"   in marks and not pi_host:    item.add_marker(skip["hardware"])
        if "cellular"   in marks and not cellular:   item.add_marker(skip["cellular"])
        if "disruptive" in marks and not disruptive: item.add_marker(skip["disruptive"])
        if "usb_cable"  in marks and not usb_device: item.add_marker(skip["usb_cable"])


# ── SSH helper ────────────────────────────────────────────────────────────────

class SSHRunner:
    """Thin SSH wrapper used by hardware/WiFi/USB tests."""

    def __init__(self, host: str):
        self.host = host
        self._base = ["ssh"] + _SSH_OPTS + [host]

    def run(self, cmd: str, timeout: int = 30) -> subprocess.CompletedProcess:
        """Run *cmd* on the Pi; never raises on non-zero exit."""
        return subprocess.run(
            self._base + [cmd],
            capture_output=True, text=True, timeout=timeout,
        )

    def check(self, cmd: str, timeout: int = 30) -> subprocess.CompletedProcess:
        """Run *cmd*; raise ``AssertionError`` if it exits non-zero."""
        r = self.run(cmd, timeout=timeout)
        assert r.returncode == 0, (
            f"SSH command failed (exit {r.returncode}):\n"
            f"  {cmd!r}\n"
            f"  stdout: {r.stdout.strip()!r}\n"
            f"  stderr: {r.stderr.strip()!r}"
        )
        return r

    def python(self, code: str, timeout: int = 30) -> subprocess.CompletedProcess:
        """Run a Python snippet on the Pi via ``python3 -c``."""
        return self.check(f"python3 -c {code!r}", timeout=timeout)

    def script(self, code: str, timeout: int = 30) -> subprocess.CompletedProcess:
        """Run a multiline Python script on the Pi by piping it to python3 stdin."""
        return subprocess.run(
            self._base + ["python3 -"],
            input=code, capture_output=True, text=True, timeout=timeout,
        )


# ── Session-scoped fixtures ───────────────────────────────────────────────────

@pytest.fixture(scope="session")
def ssh(request):
    """
    SSHRunner connected to the Pi.  Session-scoped so SSH is verified once.
    Auto-skips the test if the Pi is unreachable.
    """
    host = request.config.getoption("--pi-host") or DEFAULT_HOST
    runner = SSHRunner(host)
    r = runner.run("echo pong", timeout=8)
    if r.returncode != 0 or "pong" not in r.stdout:
        pytest.skip(f"Pi not reachable at {host}: {r.stderr.strip() or 'timeout'}")
    return runner


@pytest.fixture(scope="session")
def pi_config(ssh):
    """Load config.yaml from the Pi (tries ~/config.yaml then the project path)."""
    r = ssh.run(
        "cat ~/config.yaml 2>/dev/null "
        "|| cat /home/cedric/USBCellular/config.yaml 2>/dev/null"
    )
    assert r.returncode == 0 and r.stdout.strip(), \
        "config.yaml not found on Pi – deploy with: scp config.yaml cedric@pi:~"
    return yaml.safe_load(r.stdout)


@pytest.fixture(scope="session")
def usb_device(request):
    """USB block device path from --usb-device (auto-skips if not supplied)."""
    dev = request.config.getoption("--usb-device")
    if not dev:
        pytest.skip("pass --usb-device /dev/sdX to enable USB-cable tests")
    return dev

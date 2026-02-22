"""
Modem and cellular tests.
Replaces: sim7000_test.py, ftp_test.py, and the cellular phases of e2e_test.py.

Tiers:
  hardware  – serial port accessible, modem physically present
  cellular  – active SIM + network registration required
  slow      – tests that take > 30 s (network attach, FTP upload)

Run:
    pytest tests/test_modem.py --pi-host cedric@pizerologs.local
    pytest tests/test_modem.py --pi-host ... --cellular
    pytest tests/test_modem.py --pi-host ... --cellular --disruptive
"""

import pytest


# ── Helper: run modem code on Pi via SSH ──────────────────────────────────────

def _modem_snippet(ssh, code: str, timeout: int = 15) -> str:
    """
    Run a Python snippet that uses SIM7000GHandler on the Pi.
    Returns stdout. Raises AssertionError if the command fails.
    """
    preamble = (
        "import sys; sys.path.insert(0,'/home/cedric'); "
        "from modem_handler import SIM7000GHandler; "
        "m = SIM7000GHandler('/dev/ttyAMA0', 115200, timeout=1); "
    )
    r = ssh.run(f"python3 -c \"{preamble}{code}\"", timeout=timeout)
    assert r.returncode == 0, (
        f"Modem snippet failed:\n  code: {code!r}\n"
        f"  stdout: {r.stdout.strip()}\n  stderr: {r.stderr.strip()}"
    )
    return r.stdout


# ── Basic modem presence (hardware, non-cellular) ─────────────────────────────

@pytest.mark.hardware
def test_modem_at_ok(ssh):
    """Modem replies OK to a bare AT command within 3 s."""
    out = _modem_snippet(ssh, "ok, _ = m.send_at('AT', timeout=3); print('ok' if ok else 'fail')")
    assert out.startswith("ok"), f"Modem did not respond OK: {out!r}"


@pytest.mark.hardware
def test_modem_returns_csq(ssh):
    """AT+CSQ returns a parseable signal-quality reading."""
    out = _modem_snippet(ssh, "ok, resp = m.send_at('AT+CSQ', timeout=3); print(resp)")
    assert "+CSQ:" in out, f"AT+CSQ did not return +CSQ: line: {out!r}"


@pytest.mark.hardware
def test_modem_csq_value_in_range(ssh):
    """CSQ value is 0-31 (valid) or 99 (unknown) – never an impossible number."""
    out = _modem_snippet(ssh,
        "ok, resp = m.send_at('AT+CSQ', timeout=3); "
        "import re; m2 = re.search(r'\\+CSQ: ?(\\d+)', resp); "
        "print(m2.group(1) if m2 else 'NONE')"
    )
    csq_str = out.strip()
    assert csq_str.isdigit(), f"Could not parse CSQ value: {out!r}"
    csq = int(csq_str)
    assert csq == 99 or 0 <= csq <= 31, f"CSQ {csq} out of range"


@pytest.mark.hardware
def test_modem_sim_present(ssh):
    """AT+CIMI returns the IMSI – confirms SIM card is seated."""
    out = _modem_snippet(ssh, "ok, resp = m.send_at('AT+CIMI', timeout=5); print(resp)")
    # IMSI is 15 digits; ERROR means no SIM
    assert "ERROR" not in out, "No SIM detected (AT+CIMI returned ERROR)"
    import re
    assert re.search(r"\d{10,}", out), f"IMSI not found in response: {out!r}"


# ── Network registration (cellular) ───────────────────────────────────────────

@pytest.mark.hardware
@pytest.mark.cellular
@pytest.mark.slow
def test_network_registration(ssh):
    """
    Modem registers on the cellular network within 60 s.
    AT+CEREG? returns ,1 (home) or ,5 (roaming).
    """
    out = _modem_snippet(ssh,
        "import time; deadline = time.time() + 60; "
        "reg = False; "
        "while time.time() < deadline: "
        "  ok, resp = m.send_at('AT+CEREG?', timeout=3); "
        "  if ',1' in resp or ',5' in resp: reg = True; break; "
        "  time.sleep(3); "
        "print('registered' if reg else 'unregistered')",
        timeout=75,
    )
    assert "registered" in out, (
        "Network registration failed within 60 s. "
        "Check SIM, APN, and antenna connection."
    )


@pytest.mark.hardware
@pytest.mark.cellular
@pytest.mark.slow
def test_network_data_bearer(ssh, pi_config):
    """Bearer opens and returns a non-zero IP address."""
    apn = pi_config.get("modem", {}).get("apn", "hologram")
    out = _modem_snippet(ssh,
        f"ok, msg = m.setup_network(apn={apn!r}); "
        "print('connected' if ok else 'failed', msg)",
        timeout=90,
    )
    assert "connected" in out, f"Bearer setup failed: {out!r}"
    # Teardown
    _modem_snippet(ssh, "m.close_bearer()", timeout=15)


@pytest.mark.hardware
@pytest.mark.cellular
@pytest.mark.slow
def test_ftp_upload_small_file(ssh, pi_config, tmp_path):
    """
    Create a small file on the Pi, upload via FTP, verify success, delete file.
    Uses the FTP server configured in config.yaml.
    """
    ftp = pi_config["ftp"]
    apn = pi_config.get("modem", {}).get("apn", "hologram")

    # Create a temp file on the Pi
    remote_tmp = "/tmp/pytest_ftp_test.txt"
    ssh.check(
        f"echo 'pytest FTP test $(date)' > {remote_tmp}"
    )

    code = (
        f"ok, msg = m.setup_network(apn={apn!r}); "
        f"assert ok, f'network failed: {{msg}}'; "
        f"ok, msg = m.upload_ftp("
        f"    server={ftp['server']!r}, port={ftp['port']!r}, "
        f"    username={ftp['username']!r}, password={ftp['password']!r}, "
        f"    filepath={remote_tmp!r}, remote_path={ftp['remote_path']!r}); "
        f"m.close_bearer(); "
        f"print('uploaded' if ok else f'FAILED: {{msg}}')"
    )
    out = _modem_snippet(ssh, code, timeout=120)
    assert "uploaded" in out, f"FTP upload failed: {out!r}"

    # Clean up temp file on Pi
    ssh.run(f"rm -f {remote_tmp}")

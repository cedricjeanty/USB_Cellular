"""
Unit tests – no hardware required.
Covers: config validation, dependency imports, display helper functions,
        wifi_manager pure helpers.

Run anywhere:
    pytest tests/test_unit.py
"""

import os
import sys
import pytest
import yaml

# Allow importing airbridge modules from either the dev project tree
# or the Pi's flat home-directory deployment.
_HERE = os.path.dirname(__file__)
for _p in (
    os.path.join(_HERE, "..", "src", "airbridge"),
    os.path.expanduser("~"),
):
    if os.path.isdir(_p) and _p not in sys.path:
        sys.path.insert(0, _p)

_CONFIG_SEARCH = [
    os.path.join(_HERE, "..", "config.yaml"),
    os.path.expanduser("~/config.yaml"),
]


# ── Config fixture ────────────────────────────────────────────────────────────

@pytest.fixture(scope="module")
def cfg():
    for path in _CONFIG_SEARCH:
        if os.path.exists(path):
            with open(path) as f:
                return yaml.safe_load(f)
    pytest.skip("config.yaml not found locally – copy from Pi or run on Pi")


# ── Config structure ──────────────────────────────────────────────────────────

def test_config_loads(cfg):
    assert isinstance(cfg, dict)


@pytest.mark.parametrize("key", [
    "virtual_disk_path",
    "mount_point",
    "outbox_dir",
    "quiet_window_seconds",
    "poll_interval",
])
def test_config_top_level_key_present(cfg, key):
    assert key in cfg, f"Missing top-level config key: {key!r}"


@pytest.mark.parametrize("key", ["server", "port", "username", "password", "remote_path"])
def test_config_ftp_key_present(cfg, key):
    assert "ftp" in cfg, "Missing 'ftp' section"
    assert key in cfg["ftp"], f"Missing ftp.{key}"


@pytest.mark.parametrize("key", ["ssid", "channel"])
def test_config_wifi_ap_key_present(cfg, key):
    assert "wifi_ap" in cfg, "Missing 'wifi_ap' section"
    assert key in cfg["wifi_ap"], f"Missing wifi_ap.{key}"


def test_config_quiet_window_positive(cfg):
    assert cfg["quiet_window_seconds"] > 0


def test_config_poll_interval_positive(cfg):
    assert cfg["poll_interval"] > 0


def test_config_virtual_disk_path_nonempty(cfg):
    assert cfg["virtual_disk_path"], "virtual_disk_path must not be empty"


def test_config_ftp_port_valid(cfg):
    port = cfg["ftp"]["port"]
    assert isinstance(port, int)
    assert 1 <= port <= 65535, f"FTP port out of range: {port}"


def test_config_wifi_ap_channel_valid(cfg):
    channel = cfg["wifi_ap"]["channel"]
    assert isinstance(channel, int)
    assert 1 <= channel <= 14, f"WiFi channel out of range: {channel}"


# ── Dependency imports ────────────────────────────────────────────────────────

@pytest.mark.parametrize("module", ["yaml", "zipfile", "subprocess", "socket"])
def test_python_dependency_importable(module):
    __import__(module)


def test_import_wifi_manager():
    from wifi_manager import is_connected, get_wifi_info, rssi_to_csq  # noqa: F401


def test_import_display_handler():
    from display_handler import AirbridgeDisplay  # noqa: F401


# ── wifi_manager pure helpers ─────────────────────────────────────────────────

@pytest.fixture(scope="module")
def wm():
    import wifi_manager as m
    return m


@pytest.mark.parametrize("dbm, expected_csq", [
    (None, 99),   # unknown
    (-30,  31),   # excellent (clamp to max)
    (-60,  15),   # mid-range
    (-90,   0),   # minimum
    (-100,  0),   # below minimum (clamp to 0)
    (-51,  20),   # mid-high
])
def test_rssi_to_csq(wm, dbm, expected_csq):
    assert wm.rssi_to_csq(dbm) == expected_csq


def test_rssi_to_csq_none_is_99(wm):
    assert wm.rssi_to_csq(None) == 99


def test_rssi_to_csq_max_clamped(wm):
    assert wm.rssi_to_csq(0) == 31   # above -30 clamps to 31


def test_rssi_to_csq_min_clamped(wm):
    assert wm.rssi_to_csq(-200) == 0  # far below -90 clamps to 0


# ── display_handler pure-Python helpers ───────────────────────────────────────

@pytest.fixture(scope="module")
def dh():
    """Import display helper functions once for the whole module."""
    import display_handler as m
    return m


# _csq_to_bars ----------------------------------------------------------------

@pytest.mark.parametrize("csq, bars", [
    (99, 0),   # unknown
    (-1, 0),   # invalid
    ( 0, 0),   # silent
    ( 4, 0),
    ( 5, 1),
    ( 9, 1),
    (10, 2),
    (14, 2),
    (15, 3),
    (19, 3),
    (20, 4),
    (31, 4),   # max
])
def test_csq_to_bars(dh, csq, bars):
    assert dh._csq_to_bars(csq) == bars


# _csq_to_dbm -----------------------------------------------------------------

@pytest.mark.parametrize("csq, dbm", [
    (99, None),
    ( 0, -113),
    ( 1, -111),
    (10,  -93),
    (31,  -51),
])
def test_csq_to_dbm(dh, csq, dbm):
    assert dh._csq_to_dbm(csq) == dbm


# _fmt_size -------------------------------------------------------------------

@pytest.mark.parametrize("mb, suffix", [
    (0.0,    "KB"),
    (0.05,   "KB"),
    (0.1,    "MB"),
    (1.5,    "MB"),
    (999.9,  "MB"),
    (1024.0, "GB"),
    (2048.0, "GB"),
])
def test_fmt_size_suffix(dh, mb, suffix):
    result = dh._fmt_size(mb)
    assert result.endswith(suffix), f"_fmt_size({mb}) = {result!r}, expected suffix {suffix!r}"


def test_fmt_size_nonzero_value(dh):
    result = dh._fmt_size(12.3)
    assert "12.3" in result


# _STRETCH lookup table -------------------------------------------------------

def test_stretch_table_has_256_entries(dh):
    assert len(dh._STRETCH) == 256


def test_stretch_all_zeros(dh):
    top, bot = dh._STRETCH[0x00]
    assert top == 0x00 and bot == 0x00


def test_stretch_all_ones(dh):
    top, bot = dh._STRETCH[0xFF]
    assert top == 0xFF and bot == 0xFF


@pytest.mark.parametrize("b, exp_top, exp_bot", [
    (0x01, 0x03, 0x00),   # bit 0 → top page bits 0-1
    (0x02, 0x0C, 0x00),   # bit 1 → top page bits 2-3
    (0x10, 0x00, 0x03),   # bit 4 → bot page bits 0-1
    (0x80, 0x00, 0xC0),   # bit 7 → bot page bits 6-7
])
def test_stretch_single_bit(dh, b, exp_top, exp_bot):
    top, bot = dh._STRETCH[b]
    assert top == exp_top, f"_STRETCH[{b:#04x}] top: {top:#04x} != {exp_top:#04x}"
    assert bot == exp_bot, f"_STRETCH[{b:#04x}] bot: {bot:#04x} != {exp_bot:#04x}"


def test_stretch_is_reversible(dh):
    """Every stretched byte pair reconstructs the original 8 bits."""
    for b in range(256):
        top, bot = dh._STRETCH[b]
        reconstructed = 0
        for i in range(4):
            reconstructed |= ((top >> (2 * i)) & 1) << i
        for i in range(4):
            reconstructed |= ((bot >> (2 * i)) & 1) << (i + 4)
        assert reconstructed == b, f"stretch/reconstruct mismatch for {b:#04x}"

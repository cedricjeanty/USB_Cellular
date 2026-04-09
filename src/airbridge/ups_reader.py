"""
ups_reader.py - INA219 battery monitor for Waveshare UPS HAT (C).

I2C bus 1, address 0x43 (A0 tied high, A1 tied low on the Waveshare board).
Shunt resistor: 100 mΩ on-board.
Bus voltage V+: raw LiPo cell voltage (~3.0–4.2 V, measured before TPS61088
boost converter), so bus voltage reading directly tracks battery state.
"""

import logging

log = logging.getLogger("airbridge.ups")

# ── INA219 register map ────────────────────────────────────────────────────────
_REG_CONFIG  = 0x00
_REG_SHUNT_V = 0x01
_REG_BUS_V   = 0x02
_REG_CURRENT = 0x04
_REG_CALIB   = 0x05

# Default config: 32V FSR, PGA÷8 (±320 mV), 12-bit ADC, continuous conversion.
# 0x399F is the INA219 power-on default; we re-write it explicitly so the
# calibration register that follows is applied to a known config state.
_CONFIG_VALUE = 0x399F

# Calibration for 100 mΩ shunt with 0.1 mA current LSB:
#   Cal = trunc(0.04096 / (I_LSB × R_shunt))
#       = trunc(0.04096 / (0.0001 × 0.1)) = 4096
_CALIB_VALUE   = 4096
_CURRENT_LSB_A = 0.0001   # 0.1 mA per current-register LSB

# ── LiPo single-cell discharge curve: (cell_voltage_V, state_of_charge_%) ─────
# Derived from a typical 3.7 V/4.2 V LiPo discharge profile at ~0.5 C.
_LIPO_TABLE = [
    (4.20, 100), (4.15, 95), (4.11, 90), (4.08, 85),
    (4.02, 80),  (3.98, 75), (3.95, 70), (3.91, 65),
    (3.87, 60),  (3.83, 55), (3.79, 50), (3.75, 45),
    (3.71, 40),  (3.67, 35), (3.63, 30), (3.59, 25),
    (3.55, 20),  (3.50, 15), (3.43, 10), (3.33,  5),
    (3.00,   0),
]


def _voltage_to_pct(v: float) -> int:
    """Interpolate battery percentage from cell voltage using the LiPo table."""
    if v >= _LIPO_TABLE[0][0]:
        return 100
    if v <= _LIPO_TABLE[-1][0]:
        return 0
    for i in range(len(_LIPO_TABLE) - 1):
        v_hi, p_hi = _LIPO_TABLE[i]
        v_lo, p_lo = _LIPO_TABLE[i + 1]
        if v_lo <= v <= v_hi:
            ratio = (v - v_lo) / (v_hi - v_lo)
            return round(p_lo + ratio * (p_hi - p_lo))
    return 0


class UPSReader:
    """
    Read battery voltage, current, and state-of-charge from the INA219 on
    the Waveshare UPS HAT (C).

    All I2C errors are swallowed and logged at DEBUG level so the main
    service never crashes due to a missing or temporarily inaccessible UPS.

    Usage::

        ups = UPSReader()
        result = ups.read()       # (voltage_V, current_mA, pct) or None
        pct    = ups.battery_pct()  # int 0-100 or None
    """

    I2C_ADDR = 0x43

    def __init__(self, bus: int = 1, addr: int = I2C_ADDR):
        self._addr  = addr
        self._smbus = None
        try:
            try:
                import smbus                  # python3-smbus (preferred)
            except ImportError:
                import smbus2 as smbus        # smbus2 fallback
            self._smbus = smbus.SMBus(bus)
            self._write(_REG_CONFIG, _CONFIG_VALUE)
            self._write(_REG_CALIB,  _CALIB_VALUE)
            log.info(f"INA219 UPS ready at I2C-{bus} 0x{addr:02X}")
        except Exception as exc:
            self._smbus = None               # ensure available == False on failure
            log.warning(f"UPS unavailable ({exc}); battery display disabled")

    @property
    def available(self) -> bool:
        return self._smbus is not None

    # ── I2C helpers ────────────────────────────────────────────────────────────

    def _write(self, reg: int, value: int):
        """Write a 16-bit big-endian value to an INA219 register."""
        self._smbus.write_i2c_block_data(
            self._addr, reg, [(value >> 8) & 0xFF, value & 0xFF])

    def _read_signed(self, reg: int) -> int:
        """Read a signed 16-bit big-endian value from an INA219 register."""
        raw = self._smbus.read_i2c_block_data(self._addr, reg, 2)
        val = (raw[0] << 8) | raw[1]
        return val - 0x10000 if val > 0x7FFF else val

    # ── Public API ─────────────────────────────────────────────────────────────

    def read(self):
        """
        Return ``(voltage_V, current_mA, pct)`` or ``None`` on any error.

        * ``voltage_V``  – LiPo cell voltage (3.0–4.2 V typical)
        * ``current_mA`` – positive = charging; negative = discharging
        * ``pct``        – estimated state of charge, 0–100 %
        """
        if not self.available:
            return None
        try:
            bus_raw    = self._read_signed(_REG_BUS_V)
            voltage    = ((bus_raw >> 3) & 0x1FFF) * 4e-3   # 4 mV per LSB
            cur_raw    = self._read_signed(_REG_CURRENT)
            current_mA = cur_raw * _CURRENT_LSB_A * 1000
            pct        = _voltage_to_pct(voltage)
            log.debug(f"UPS: {voltage:.3f} V  {current_mA:+.1f} mA  {pct}%")
            return voltage, current_mA, pct
        except Exception as exc:
            log.debug(f"UPS read error: {exc}")
            return None

    def battery_pct(self):
        """Return battery percentage (0–100) or ``None`` on error."""
        result = self.read()
        return result[2] if result is not None else None

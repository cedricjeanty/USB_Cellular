"""
display_handler.py - SSD1306 128×64 OLED status display for USB Airbridge.

Dependencies: python3-smbus or smbus2 (no Pillow / luma.oled needed).
I2C bus 1, address 0x3C.

Display layout (8 pages × 8 px = 64 px):

  Page 0 │ T-Mobile Hologram  ▐▐▐░ │  carrier (≤18 chars) + signal bars flush-right
  Page 1 │                         │  blank spacer
  Page 2 │ ╮ USB ACTIVE            │  2× tall text
  Page 3 │ ╯                       │
  Page 4 │ USB [==========] 14.2MB │  drive capacity bar
  Page 5 │                         │  blank spacer
  Page 6 │ UP0.4MB[======]  R0.0MB │  upload progress (always shown, rem flush-right)
  Page 7 │                         │  blank
"""

import logging

log = logging.getLogger("airbridge.display")

# ── I2C / SSD1306 constants ────────────────────────────────────────────────────
I2C_BUS   = 1
OLED_ADDR = 0x3C   # SA0 low; change to 0x3D if SA0 pin is pulled high

REG_CMD  = 0x00    # following bytes are commands
REG_DATA = 0x40    # following bytes are GDDRAM data

COLS  = 128
PAGES = 8          # 8 pages × 8 rows = 64 px

# ── SSD1306 init sequence ──────────────────────────────────────────────────────
_INIT = [
    0xAE,        # display off
    0xD5, 0x80,  # clock
    0xA8, 0x3F,  # mux ratio: 64 rows
    0xD3, 0x00,  # display offset: 0
    0x40,        # start line: 0
    0x8D, 0x14,  # charge pump on
    0x20, 0x00,  # horizontal addressing mode
    0xA1,        # seg remap (col127→SEG0, flip horizontal)
    0xC8,        # COM scan remapped (flip vertical)
    0xDA, 0x12,  # COM pins: alt, no L/R remap
    0x81, 0xCF,  # contrast
    0xD9, 0xF1,  # pre-charge
    0xDB, 0x40,  # Vcomh
    0xA4,        # use RAM
    0xA6,        # normal (non-inverted)
    0xAF,        # display on
]

# ── 5×8 bitmap font, printable ASCII 0x20–0x7E ───────────────────────────────
# Each char = 5 column bytes; bit 0 = top pixel of column, bit 7 = bottom.
CHAR_W = 5
CHAR_H = 8
GAP    = 1
CELL_W = CHAR_W + GAP   # 6 px per glyph → 21 chars per 128-px row

_FONT = (
    b'\x00\x00\x00\x00\x00'  # 0x20  ' '
    b'\x00\x00\x5F\x00\x00'  # 0x21  '!'
    b'\x00\x07\x00\x07\x00'  # 0x22  '"'
    b'\x14\x7F\x14\x7F\x14'  # 0x23  '#'
    b'\x24\x2A\x7F\x2A\x12'  # 0x24  '$'
    b'\x23\x13\x08\x64\x62'  # 0x25  '%'
    b'\x36\x49\x55\x22\x50'  # 0x26  '&'
    b'\x00\x05\x03\x00\x00'  # 0x27  "'"
    b'\x00\x1C\x22\x41\x00'  # 0x28  '('
    b'\x00\x41\x22\x1C\x00'  # 0x29  ')'
    b'\x14\x08\x3E\x08\x14'  # 0x2A  '*'
    b'\x08\x08\x3E\x08\x08'  # 0x2B  '+'
    b'\x00\x50\x30\x00\x00'  # 0x2C  ','
    b'\x08\x08\x08\x08\x08'  # 0x2D  '-'
    b'\x00\x60\x60\x00\x00'  # 0x2E  '.'
    b'\x20\x10\x08\x04\x02'  # 0x2F  '/'
    b'\x3E\x51\x49\x45\x3E'  # 0x30  '0'
    b'\x00\x42\x7F\x40\x00'  # 0x31  '1'
    b'\x42\x61\x51\x49\x46'  # 0x32  '2'
    b'\x21\x41\x45\x4B\x31'  # 0x33  '3'
    b'\x18\x14\x12\x7F\x10'  # 0x34  '4'
    b'\x27\x45\x45\x45\x39'  # 0x35  '5'
    b'\x3C\x4A\x49\x49\x30'  # 0x36  '6'
    b'\x01\x71\x09\x05\x03'  # 0x37  '7'
    b'\x36\x49\x49\x49\x36'  # 0x38  '8'
    b'\x06\x49\x49\x29\x1E'  # 0x39  '9'
    b'\x00\x36\x36\x00\x00'  # 0x3A  ':'
    b'\x00\x56\x36\x00\x00'  # 0x3B  ';'
    b'\x08\x14\x22\x41\x00'  # 0x3C  '<'
    b'\x14\x14\x14\x14\x14'  # 0x3D  '='
    b'\x00\x41\x22\x14\x08'  # 0x3E  '>'
    b'\x02\x01\x51\x09\x06'  # 0x3F  '?'
    b'\x32\x49\x79\x41\x3E'  # 0x40  '@'
    b'\x7E\x11\x11\x11\x7E'  # 0x41  'A'
    b'\x7F\x49\x49\x49\x36'  # 0x42  'B'
    b'\x3E\x41\x41\x41\x22'  # 0x43  'C'
    b'\x7F\x41\x41\x22\x1C'  # 0x44  'D'
    b'\x7F\x49\x49\x49\x41'  # 0x45  'E'
    b'\x7F\x09\x09\x09\x01'  # 0x46  'F'
    b'\x3E\x41\x49\x49\x7A'  # 0x47  'G'
    b'\x7F\x08\x08\x08\x7F'  # 0x48  'H'
    b'\x00\x41\x7F\x41\x00'  # 0x49  'I'
    b'\x20\x40\x41\x3F\x01'  # 0x4A  'J'
    b'\x7F\x08\x14\x22\x41'  # 0x4B  'K'
    b'\x7F\x40\x40\x40\x40'  # 0x4C  'L'
    b'\x7F\x02\x0C\x02\x7F'  # 0x4D  'M'
    b'\x7F\x04\x08\x10\x7F'  # 0x4E  'N'
    b'\x3E\x41\x41\x41\x3E'  # 0x4F  'O'
    b'\x7F\x09\x09\x09\x06'  # 0x50  'P'
    b'\x3E\x41\x51\x21\x5E'  # 0x51  'Q'
    b'\x7F\x09\x19\x29\x46'  # 0x52  'R'
    b'\x46\x49\x49\x49\x31'  # 0x53  'S'
    b'\x01\x01\x7F\x01\x01'  # 0x54  'T'
    b'\x3F\x40\x40\x40\x3F'  # 0x55  'U'
    b'\x1F\x20\x40\x20\x1F'  # 0x56  'V'
    b'\x3F\x40\x38\x40\x3F'  # 0x57  'W'
    b'\x63\x14\x08\x14\x63'  # 0x58  'X'
    b'\x07\x08\x70\x08\x07'  # 0x59  'Y'
    b'\x61\x51\x49\x45\x43'  # 0x5A  'Z'
    b'\x00\x7F\x41\x41\x00'  # 0x5B  '['
    b'\x02\x04\x08\x10\x20'  # 0x5C  '\'
    b'\x00\x41\x41\x7F\x00'  # 0x5D  ']'
    b'\x04\x02\x01\x02\x04'  # 0x5E  '^'
    b'\x40\x40\x40\x40\x40'  # 0x5F  '_'
    b'\x00\x01\x02\x04\x00'  # 0x60  '`'
    b'\x20\x54\x54\x54\x78'  # 0x61  'a'
    b'\x7F\x48\x44\x44\x38'  # 0x62  'b'
    b'\x38\x44\x44\x44\x20'  # 0x63  'c'
    b'\x38\x44\x44\x48\x7F'  # 0x64  'd'
    b'\x38\x54\x54\x54\x18'  # 0x65  'e'
    b'\x08\x7E\x09\x01\x02'  # 0x66  'f'
    b'\x0C\x52\x52\x52\x3E'  # 0x67  'g'
    b'\x7F\x08\x04\x04\x78'  # 0x68  'h'
    b'\x00\x44\x7D\x40\x00'  # 0x69  'i'
    b'\x20\x40\x44\x3D\x00'  # 0x6A  'j'
    b'\x7F\x10\x28\x44\x00'  # 0x6B  'k'
    b'\x00\x41\x7F\x40\x00'  # 0x6C  'l'
    b'\x7C\x04\x18\x04\x78'  # 0x6D  'm'
    b'\x7C\x08\x04\x04\x78'  # 0x6E  'n'
    b'\x38\x44\x44\x44\x38'  # 0x6F  'o'
    b'\x7C\x14\x14\x14\x08'  # 0x70  'p'
    b'\x08\x14\x14\x18\x7C'  # 0x71  'q'
    b'\x7C\x08\x04\x04\x08'  # 0x72  'r'
    b'\x48\x54\x54\x54\x20'  # 0x73  's'
    b'\x04\x3F\x44\x40\x20'  # 0x74  't'
    b'\x3C\x40\x40\x20\x7C'  # 0x75  'u'
    b'\x1C\x20\x40\x20\x1C'  # 0x76  'v'
    b'\x3C\x40\x30\x40\x3C'  # 0x77  'w'
    b'\x44\x28\x10\x28\x44'  # 0x78  'x'
    b'\x0C\x50\x50\x50\x3C'  # 0x79  'y'
    b'\x44\x64\x54\x4C\x44'  # 0x7A  'z'
    b'\x00\x08\x36\x41\x00'  # 0x7B  '{'
    b'\x00\x00\x7F\x00\x00'  # 0x7C  '|'
    b'\x00\x41\x36\x08\x00'  # 0x7D  '}'
    b'\x10\x08\x08\x10\x08'  # 0x7E  '~'
)

# ── 2× vertical stretch lookup: _STRETCH[b] = (top_byte, bot_byte) ────────────
# Each bit in a font column byte becomes 2 adjacent bits; the 8 resulting bits
# split into a top-page byte (from original bits 0-3) and bottom-page byte
# (from original bits 4-7).  Bit 0 = topmost pixel of a page.
_STRETCH = []
for _b in range(256):
    _t = _bt = 0
    for _i in range(4):
        _bit = (_b >> _i) & 1
        _t  |= _bit << (2 * _i) | _bit << (2 * _i + 1)
    for _i in range(4):
        _bit = (_b >> (_i + 4)) & 1
        _bt |= _bit << (2 * _i) | _bit << (2 * _i + 1)
    _STRETCH.append((_t, _bt))


# ─── Low-level SSD1306 driver ──────────────────────────────────────────────────

class SSD1306:
    """Minimal SSD1306 OLED driver. Uses only python3-smbus."""

    def __init__(self, bus=I2C_BUS, addr=OLED_ADDR):
        try:
            import smbus  # python3-smbus
        except ImportError:
            import smbus2 as smbus  # fallback (Pi has smbus2 installed)
        # Pi-only; deferred so pure helpers are importable anywhere
        self._bus  = smbus.SMBus(bus)
        self._addr = addr
        self._buf  = bytearray(COLS * PAGES)  # 1 KB frame-buffer
        self._cmd(_INIT)

    # ── I2C ────────────────────────────────────────────────────────────────────

    def _cmd(self, cmds):
        for i in range(0, len(cmds), 31):
            self._bus.write_i2c_block_data(
                self._addr, REG_CMD, list(cmds[i:i + 31]))

    def _data(self, buf):
        mv = memoryview(buf)
        for i in range(0, len(buf), 31):
            self._bus.write_i2c_block_data(
                self._addr, REG_DATA, list(mv[i:i + 31]))

    # ── Frame-buffer ───────────────────────────────────────────────────────────

    def flush(self):
        self._cmd([0x21, 0x00, 0x7F])   # columns 0–127
        self._cmd([0x22, 0x00, 0x07])   # pages 0–7
        self._data(self._buf)

    def clear(self, flush=True):
        for i in range(len(self._buf)):
            self._buf[i] = 0
        if flush:
            self.flush()

    def fill_page(self, page, byte=0x00):
        start = page * COLS
        for i in range(COLS):
            self._buf[start + i] = byte

    # ── 1× text ────────────────────────────────────────────────────────────────

    def draw_char(self, x, page, ch):
        code = ord(ch) if isinstance(ch, str) else ch
        if not (0x20 <= code <= 0x7E):
            code = 0x20
        idx  = (code - 0x20) * CHAR_W
        base = page * COLS + x
        for k, byte in enumerate(_FONT[idx:idx + CHAR_W]):
            pos = base + k
            if 0 <= pos < len(self._buf):
                self._buf[pos] = byte

    def text(self, x, page, s, width=None):
        """Write string at (x, page). Pad/truncate to width chars if given."""
        if width is not None:
            s = s[:width].ljust(width)
        for ch in s:
            if x + CHAR_W > COLS:
                break
            self.draw_char(x, page, ch)
            x += CELL_W

    # ── 2× text (spans page and page+1) ───────────────────────────────────────

    CHAR2_W = CHAR_W * 2 + 2   # 12 px per glyph at 2× scale

    def draw_char_2x(self, x, page, ch):
        """Render one character at 2× scale, writing into page and page+1."""
        code = ord(ch) if isinstance(ch, str) else ch
        if not (0x20 <= code <= 0x7E):
            code = 0x20
        idx = (code - 0x20) * CHAR_W
        for k in range(CHAR_W):
            top, bot = _STRETCH[_FONT[idx + k]]
            for dx in range(2):           # each column doubled horizontally
                px = x + k * 2 + dx
                if px < COLS:
                    self._buf[page * COLS + px] = top
                    if page + 1 < PAGES:
                        self._buf[(page + 1) * COLS + px] = bot

    def text_2x(self, x, page, s):
        """Write a 2× scaled string starting at (x, page)."""
        for ch in s:
            if x + CHAR_W * 2 > COLS:
                break
            self.draw_char_2x(x, page, ch)
            x += self.CHAR2_W

    def text_2x_centered(self, page, s):
        """Write 2× text centred horizontally across the full display."""
        total_w = len(s) * self.CHAR2_W
        x = max(0, (COLS - total_w) // 2)
        self.text_2x(x, page, s)

    # ── Pixel / line helpers ───────────────────────────────────────────────────

    def pixel(self, x, page, bit, on=True):
        idx = page * COLS + x
        if 0 <= idx < len(self._buf):
            if on:
                self._buf[idx] |=  (1 << bit)
            else:
                self._buf[idx] &= ~(1 << bit)

    def hline(self, page, bit_row, x0=0, x1=127):
        mask  = 1 << bit_row
        start = page * COLS
        for x in range(x0, min(x1 + 1, COLS)):
            self._buf[start + x] |= mask

    def power_off(self):
        self._cmd([0xAE])

    def power_on(self):
        self._cmd([0xAF])


# ─── Helper functions ──────────────────────────────────────────────────────────

def _csq_to_bars(csq):
    """Map CSQ value (0–31; 99=unknown) to 0–4 signal bars."""
    if csq < 0 or csq == 99: return 0
    if csq < 5:               return 0
    if csq < 10:              return 1
    if csq < 15:              return 2
    if csq < 20:              return 3
    return 4

def _csq_to_dbm(csq):
    return None if (csq == 99 or csq < 0) else -113 + 2 * csq

def _fmt_size(mb):
    """Format megabyte value as a compact string."""
    if mb >= 1000.0:     return f"{mb / 1024:.1f}GB"
    if mb >=    0.1:     return f"{mb:.1f}MB"
    return f"{mb * 1024:.0f}KB"


# ─── Pixel drawing helpers (module-level, operate on SSD1306 buf) ──────────────

def _draw_signal_bars(oled, page, x, csq):
    """
    Draw 4 signal bars at pixel column x on page.
    Bars grow from the bottom; each bar is 3 px wide + 1 px gap.
    Column byte: bit 0 = top of page, bit 7 = bottom.

    Heights (bottom-aligned):
      bar 0: 2 px → bits 6-7 set → 0xC0
      bar 1: 4 px → bits 4-7    → 0xF0
      bar 2: 6 px → bits 2-7    → 0xFC
      bar 3: 8 px → all         → 0xFF
    """
    bars    = _csq_to_bars(csq)
    FILLED  = [0xC0, 0xF0, 0xFC, 0xFF]
    OUTLINE = 0x80   # just the bottom pixel

    for b in range(4):
        val  = FILLED[b] if b < bars else OUTLINE
        base = page * COLS + x + b * 4
        for px in range(3):
            idx = base + px
            if idx < len(oled._buf):
                oled._buf[idx] = val



def _draw_progress_bar(oled, page, x, width, pct):
    """
    Draw a filled progress bar of given pixel width at (x, page).
    Bar uses the same 6-px body height as the battery icon.
    Interior width = width - 2 (1-px walls).
    """
    interior = width - 2
    fill     = round(max(0, min(100, pct)) / 100 * interior)

    FULL_COL  = 0x7E
    EMPTY_COL = 0x42
    base      = page * COLS + x

    # left cap
    idx = base
    if idx < len(oled._buf):
        oled._buf[idx] = FULL_COL

    # interior
    for i in range(interior):
        idx = base + 1 + i
        if idx < len(oled._buf):
            oled._buf[idx] = FULL_COL if i < fill else EMPTY_COL

    # right cap
    idx = base + width - 1
    if idx < len(oled._buf):
        oled._buf[idx] = FULL_COL


# ─── High-level Airbridge display ─────────────────────────────────────────────

# Pixel positions on the header row (page 0, 128 px wide)
_CARRIER_X  =  0    # carrier text: up to 18 chars (108 px)
_BARS_X     = 113   # signal bars flush-right: 4 bars × 4 px, last pixel at col 127

# USB capacity bar geometry (page 3)
_USB_LABEL_X =  0   # "USB" label
_USB_BAR_X   = 20   # bar starts 2 px after label (3 chars × 6 px + 2)

# Upload progress bar geometry (page 5) — widths computed dynamically in _refresh


class AirbridgeDisplay:
    """
    Status display for the USB Cellular Airbridge.

    Usage::

        disp = AirbridgeDisplay()
        disp.update(csq=15, carrier="T-Mobile", net_connected=True,
                    usb_active=True, mb_uploaded=12.3, mb_remaining=45.7)

    All display I/O is guarded — a missing or faulty screen never crashes
    the main service.
    """

    def __init__(self, i2c_bus=I2C_BUS, i2c_addr=OLED_ADDR):
        self._oled = None
        try:
            self._oled = SSD1306(bus=i2c_bus, addr=i2c_addr)
            self._oled.clear(flush=False)
            self._oled.flush()
            log.info(f"SSD1306 ready at I2C-{i2c_bus} 0x{i2c_addr:02X}")
        except Exception as exc:
            log.warning(f"OLED unavailable ({exc}); display disabled")

    def _ok(self):
        return self._oled is not None

    # ── Public API ─────────────────────────────────────────────────────────────

    def update(self,
               csq=99,
               carrier="",
               net_connected=False,
               usb_active=False,
               mb_uploaded=0.0,
               mb_remaining=0.0,
               drive_mb=0.0,
               drive_total_mb=0.0):
        """
        Refresh the full display with current system state.

        Args:
            csq           : Signal quality (0–31; 99 = unknown)
            carrier       : Network name or WiFi SSID
            net_connected : True when WiFi is connected
            usb_active    : True when g_mass_storage gadget is loaded
            mb_uploaded   : Megabytes uploaded this session
            mb_remaining  : Megabytes queued in outbox
            drive_mb      : Estimated MB written to virtual disk since last harvest
            drive_total_mb: Total capacity of virtual disk in MB (0 = unknown)
        """
        if not self._ok():
            return
        try:
            self._refresh(csq, carrier, net_connected, usb_active,
                          mb_uploaded, mb_remaining, drive_mb, drive_total_mb)
        except Exception as exc:
            log.warning(f"OLED update error: {exc}")

    def show_message(self, line1="", line2="", line3=""):
        """Show up to 3 free-form lines (e.g. boot status). Max 21 chars each."""
        if not self._ok():
            return
        try:
            oled = self._oled
            oled.clear(flush=False)
            oled.text(0, 1, line1[:21])
            oled.text(0, 3, line2[:21])
            oled.text(0, 5, line3[:21])
            oled.flush()
        except Exception as exc:
            log.warning(f"OLED message error: {exc}")

    def off(self):
        if self._ok():
            try:
                self._oled.power_off()
            except Exception:
                pass

    # ── Rendering ──────────────────────────────────────────────────────────────

    def _refresh(self, csq, carrier, net_connected, usb_active,
                 mb_uploaded, mb_remaining, drive_mb, drive_total_mb):
        oled = self._oled

        # ── Page 0: status bar ────────────────────────────────────────────────
        oled.fill_page(0, 0x00)

        # Carrier name / signal status — up to 18 chars before bars
        if carrier:
            left_label = carrier[:18]
        elif _csq_to_bars(csq) > 0:
            left_label = "SIGNAL"
        else:
            left_label = "NO SIGNAL"
        oled.text(_CARRIER_X, 0, left_label)

        # Signal bars flush to right edge (cols 113–127)
        _draw_signal_bars(oled, 0, _BARS_X, csq)

        # ── Page 1: blank spacer ─────────────────────────────────────────────
        oled.fill_page(1, 0x00)

        # ── Pages 2-3: USB status at 2× scale ────────────────────────────────
        oled.fill_page(2, 0x00)
        oled.fill_page(3, 0x00)
        usb_label = "USB ACTIVE" if usb_active else "USB  IDLE"
        oled.text_2x_centered(2, usb_label)

        # ── Pages 4-5: USB drive capacity bar ────────────────────────────────
        # Page 5 is cleared first; overflow from the 3-px downward nudge is
        # OR'd into it so the bottom of the bar isn't clipped.
        oled.fill_page(4, 0x00)
        oled.fill_page(5, 0x00)
        if usb_active or drive_mb > 0.0:
            oled.text(_USB_LABEL_X, 4, "USB")
            size_s = _fmt_size(drive_mb) if drive_mb > 0.0 else "--"
            size_w = len(size_s) * CELL_W
            size_x = COLS - size_w          # flush-right
            bar_w  = size_x - 2 - _USB_BAR_X
            if drive_total_mb > 0.0 and bar_w >= 4:
                drive_pct = min(100.0, drive_mb / drive_total_mb * 100.0)
                _draw_progress_bar(oled, 4, _USB_BAR_X, bar_w, drive_pct)
            oled.text(size_x, 4, size_s)
            # Nudge content down 3 pixels; carry overflow into the blank page 5.
            for _c in range(COLS):
                _orig = oled._buf[4 * COLS + _c]
                oled._buf[4 * COLS + _c] = (_orig << 3) & 0xFF
                oled._buf[5 * COLS + _c] |= (_orig >> 5) & 0xFF

        # ── Page 6: upload progress — only shown when there is something to do ──
        # "UP0.4MB[======]  R0.0MB" — remaining is flush-right; bar fills the gap.
        oled.fill_page(6, 0x00)
        if mb_uploaded > 0.0 or mb_remaining > 0.0:
            total_mb = mb_uploaded + mb_remaining
            pct   = (mb_uploaded / total_mb * 100) if total_mb > 0 else 0.0
            up_s  = "UP" + _fmt_size(mb_uploaded)
            rem_s = "R"  + _fmt_size(mb_remaining)
            bar_x = len(up_s) * CELL_W + 2     # 2 px gap after up text
            rem_x = COLS - len(rem_s) * CELL_W # flush-right
            bar_w = rem_x - 2 - bar_x          # 2 px gap before rem text
            oled.text(0,     6, up_s)
            if bar_w >= 4:
                _draw_progress_bar(oled, 6, bar_x, bar_w, pct)
            oled.text(rem_x, 6, rem_s)

        # ── Page 7: blank ────────────────────────────────────────────────────
        oled.fill_page(7, 0x00)

        oled.flush()


# ─── Standalone test ───────────────────────────────────────────────────────────

if __name__ == "__main__":
    import time, sys

    logging.basicConfig(level=logging.DEBUG, format="%(levelname)s: %(message)s")

    disp = AirbridgeDisplay()
    if not disp._ok():
        print("ERROR: Could not initialise display. Check I2C wiring.")
        sys.exit(1)

    print("Boot message...")
    disp.show_message("Airbridge v1.0", "Initialising...", "Please wait")
    time.sleep(2)

    scenarios = [
        # Idle — no signal, nothing uploaded, nothing queued
        dict(csq=99,  carrier="",         net_connected=False, usb_active=False,
             mb_uploaded=0,    mb_remaining=0,    drive_mb=0.0,  drive_total_mb=0.0),
        # USB active, files queued but upload not started yet
        dict(csq=8,   carrier="T-Mobile", net_connected=False, usb_active=True,
             mb_uploaded=0,    mb_remaining=50.0, drive_mb=14.2, drive_total_mb=256.0),
        # Uploading: 12.3 of 50 MB done (25%)
        dict(csq=14,  carrier="T-Mobile", net_connected=True,  usb_active=True,
             mb_uploaded=12.3, mb_remaining=37.7, drive_mb=8.5,  drive_total_mb=256.0),
        # Uploading: 40.1 of 50 MB done (80%)
        dict(csq=22,  carrier="T-Mobile", net_connected=True,  usb_active=False,
             mb_uploaded=40.1, mb_remaining=9.9,  drive_mb=0.0,  drive_total_mb=256.0),
        # Upload complete: 50 MB done, 0 remaining (100%)
        dict(csq=31,  carrier="T-Mobile", net_connected=True,  usb_active=False,
             mb_uploaded=50.0, mb_remaining=0,    drive_mb=0.0,  drive_total_mb=256.0),
        # Small file scenario: 52 KB uploaded, full carrier name shown
        dict(csq=19,  carrier="T-Mobile Hologram", net_connected=False, usb_active=True,
             mb_uploaded=0.052, mb_remaining=0,   drive_mb=53.7, drive_total_mb=256.0),
    ]

    for s in scenarios:
        print(f"  csq={s['csq']:2d}  net={s['net_connected']}  usb={s['usb_active']}  "
              f"up={s['mb_uploaded']:.1f}MB  left={s['mb_remaining']:.1f}MB  "
              f"drive={s['drive_mb']:.1f}MB")
        disp.update(**s)
        time.sleep(2)

    print("Done.")

#!/usr/bin/env python3
"""Remotely read the coredump partition via the power-on USB-Serial-JTAG window
(reuses jtag_flash.catch_download_mode — no BOOT button). Dumps 0x3F0000/0x10000
to /tmp/coredump.bin and reports whether it holds a valid ESP-IDF coredump image."""
import os, sys, struct, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jtag_flash

COREDUMP_OFF = 0x3F0000
COREDUMP_SIZE = 0x10000
OUT = "/tmp/coredump.bin"

port = jtag_flash.catch_download_mode("/dev/ttyACM*", manual=False)
if not port:
    sys.exit("FAILED to catch download mode")
print("== download mode on", port, "==")
rc = jtag_flash.esp(port, "--before", "no_reset", "--after", "no_reset",
                    "read_flash", hex(COREDUMP_OFF), hex(COREDUMP_SIZE), OUT)
# leave the chip; caller re-powers
subprocess.run(["python3", os.path.join(jtag_flash.ROOT, "scripts", "coolgear.py"), "off"],
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
if rc != 0:
    sys.exit("read_flash failed")
with open(OUT, "rb") as f:
    data = f.read()
# ESP-IDF coredump-to-flash layout: u32 tot_len, u32 version, then ELF ('\x7fELF')
tot_len, ver = struct.unpack("<II", data[:8])
print(f"header: tot_len={tot_len} version=0x{ver:08x}")
if tot_len == 0xFFFFFFFF or tot_len == 0 or tot_len > COREDUMP_SIZE:
    print("=> NO valid coredump image (partition blank/erased) — panic wrote no dump")
else:
    print(f"=> VALID coredump, {tot_len} bytes")
    payload = data[4:4+tot_len]
    if b"\x7fELF" in data[:64]:
        print("   (ELF format)")
    with open("/tmp/coredump.elf", "wb") as f:
        f.write(payload)
    print("   payload -> /tmp/coredump.elf")

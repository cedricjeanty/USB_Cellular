#!/usr/bin/env python3
"""
Antenna comparison helper. Reads the firmware's `SURVEY` lines (emitted in survey
mode — see the `survey` airbridge.cmd directive) off the CDC serial port and shows
a live readout plus per-antenna rolling stats.

Setup: flash esp32s3-e2e, put `cdc` + `survey` in airbridge.cmd on the USB drive,
power-cycle. Then run this and swap antennas.

Usage:
  scripts/antenna_survey.py [--port /dev/ttyACM0]
  - Press Enter to mark/label the current antenna and print its summary, then
    start a fresh sample window for the next antenna.
  - Ctrl-C to quit.

Compare antennas primarily on median RSRP (dBm, higher/less-negative = better
coverage) and SINR (dB, higher = cleaner). Watch `band`/`carrier` — only compare
samples on the same band, since the multi-carrier SIM can reselect.
"""
import argparse, glob, re, statistics, sys, threading, time

LINE = re.compile(
    r"SURVEY\s+\d+:\s+carrier=(?P<carrier>.+?)\s+band=(?P<band>\S+)\s+"
    r"RSSI=(?P<rssi>-?\d+)\s+RSRP=(?P<rsrp>-?\d+)\s+RSRQ=(?P<rsrq>-?\d+)\s+SINR=(?P<sinr>-?\d+)")


def summarize(samples, label):
    if not samples:
        print("  (no samples)"); return
    def stat(key):
        vals = [s[key] for s in samples if s[key] < 9000]  # drop MODEM_SIG_NA sentinels
        if not vals: return "n/a"
        return "med=%g mean=%.1f min=%g max=%g" % (
            statistics.median(vals), statistics.mean(vals), min(vals), max(vals))
    bands = {}
    for s in samples:
        bands[s["band"]] = bands.get(s["band"], 0) + 1
    print("\n===== %s  (%d samples) =====" % (label, len(samples)))
    print("  RSRP dBm : %s   <-- primary antenna metric" % stat("rsrp"))
    print("  SINR dB  : %s" % stat("sinr"))
    print("  RSRQ dB  : %s" % stat("rsrq"))
    print("  RSSI     : %s" % stat("rssi"))
    print("  bands    : %s" % ", ".join("%s x%d" % (b, n) for b, n in bands.items()))
    print("=" * (14 + len(label)) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    a = ap.parse_args()
    port = a.port or (sorted(glob.glob("/dev/ttyACM*")) or [None])[0]
    if not port:
        sys.exit("no /dev/ttyACM* — is the device in CDC survey mode?")
    try:
        import serial
    except ImportError:
        sys.exit("pip install pyserial")
    s = serial.Serial(port, 115200, timeout=0.5)
    print("reading %s — swap antennas; Enter = summarize+next, Ctrl-C = quit\n" % port)

    cur = []
    ant = 1
    lock = threading.Lock()

    def reader():
        nonlocal cur
        buf = b""
        while True:
            buf += s.read(256)
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                m = LINE.search(ln.decode("utf-8", "replace"))
                if not m:
                    continue
                rec = {k: (int(m.group(k)) if k not in ("carrier", "band") else m.group(k))
                       for k in ("carrier", "band", "rssi", "rsrp", "rsrq", "sinr")}
                with lock:
                    cur.append(rec)
                print("  RSRP=%-5d SINR=%-3d RSSI=%-3d %s/%s  (n=%d)" % (
                    rec["rsrp"], rec["sinr"], rec["rssi"], rec["band"],
                    rec["carrier"].split()[0], len(cur)))

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    try:
        while True:
            input()  # Enter
            with lock:
                summarize(cur, "ANTENNA %d" % ant)
                cur = []
                ant += 1
    except KeyboardInterrupt:
        with lock:
            summarize(cur, "ANTENNA %d (final)" % ant)
        print("done")


if __name__ == "__main__":
    main()

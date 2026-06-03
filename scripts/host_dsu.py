#!/usr/bin/env python3
"""
host_dsu.py — cookie-aware host-side aircraft DSU simulator for hardware E2E.

Drives a real ESP32 over USB mass storage exactly as a real EA500 DSU drives the
AirBridge: read dsuCookie.easdf from the SD card's USB-visible partition, and emit
only the flights the device has NOT already harvested (flight > cookie), into
flightHistory/, plus metrics/ and downloadReport.txt.

This is the hardware counterpart of esp32/include/sim_dsu.h. The cookie read/CRC,
flight-index, and slice logic are faithful ports. Two emit modes:

  emit  (default)  Cookie-aware SYNTHESIS. The real .eaofh fixtures only contain
                   flights 1071-1077, but the E2E suite requests arbitrary flight
                   numbers (01501, 02000, ...). So this mode synthesizes a flight
                   file at the requested --max-flight, using the same 0x4C record
                   format as scripts/e2e_unified.sh:append_eaofh_trailer, but GATED
                   on the cookie: if the device already harvested >= --max-flight,
                   nothing is emitted ("caught up") — genuinely exercising the
                   cookie->resume loop. Backs write_dsu_file on --target device.

  slice            Faithful sim_dsu.h port: build a flight index from --fixture and
                   copy the byte slice for flights (cookie, --max-flight]. Highest
                   fidelity (real DSU bytes) but limited to the fixture's flights.

Other modes: --plant-cookie F (write a flight-F cookie), --read-cookie (print the
current cookie flight), --partial F (write a crafted truncated-tail file for the
power-loss-recovery test).

USB MSC timing: in CDC+MSC the block node appears at boot but the MSC LUN reports
NOT_READY for ~90s; --mount-find retries the actual mount until it succeeds, which
naturally waits out the presentation delay. The host always mounts -> writes -> sync
-> unmounts and NEVER holds the mount during the firmware's 15s quiet-window harvest.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

EAOFH_REC_LEN = 28          # 4-byte header (EA 4C 00 1C) + 24-byte body
METRIC_SIZE = 20215
USAGE_SIZE = 640


# ── CRC-16/8005 (matches airbridge_proto.h:crc16_8005) ──────────────────────────
def crc16_8005(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x8005) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ── DSU cookie (78 bytes; matches buildDsuCookie / readCookie) ───────────────────
def build_cookie(serial: str, flight: int) -> bytes:
    c = bytearray(78)
    c[0] = 0xEA           # magic
    c[1] = 0x1E           # file type
    c[2] = 0x00; c[3] = 78  # length BE u16
    c[4] = 0xD1           # DSU hardware ID
    sb = serial.encode()[:42]
    c[9:9 + len(sb)] = sb
    c[60] = 0x01          # mode flag (flight mode)
    c[62] = (flight >> 24) & 0xFF
    c[63] = (flight >> 16) & 0xFF
    c[64] = (flight >> 8) & 0xFF
    c[65] = flight & 0xFF
    crc = crc16_8005(bytes(c[:76]))
    c[76] = (crc >> 8) & 0xFF
    c[77] = crc & 0xFF
    return bytes(c)


def read_cookie_flight(mount: str) -> int:
    """Return the flight number from <mount>/dsuCookie.easdf, or 0 if absent /
    bad magic / bad CRC / date-mode. Mirrors sim_dsu.h:readCookie."""
    path = os.path.join(mount, "dsuCookie.easdf")
    try:
        with open(path, "rb") as f:
            c = f.read(78)
    except OSError:
        return 0
    if len(c) < 78 or c[0] != 0xEA or c[1] != 0x1E:
        return 0
    computed = crc16_8005(c[:76])
    stored = (c[76] << 8) | c[77]
    if computed != stored:
        print(f"[host_dsu] cookie CRC mismatch (0x{computed:04X} vs 0x{stored:04X})")
        return 0
    flight = (c[62] << 24) | (c[63] << 16) | (c[64] << 8) | c[65]
    if flight == 0xFFFFFFFF:
        print("[host_dsu] date-mode cookie — treating as no cookie")
        return 0
    return flight


# ── 0x4C flight summary record (matches append_eaofh_trailer) ───────────────────
def make_record(serial: str, flight: int) -> bytes:
    body = bytearray(24)
    sb = serial.encode()[:12]
    body[5:5 + len(sb)] = sb
    body[20] = (flight >> 8) & 0xFF
    body[21] = flight & 0xFF
    return bytes([0xEA, 0x4C, 0x00, 0x1C]) + bytes(body)


# ── Faithful flight index (port of sim_dsu.h:buildFlightIndex) ──────────────────
def build_flight_index(path: str):
    """Return [(flight, block_start, block_end, serial), ...] for a .eaofh file."""
    index = []
    size = os.path.getsize(path)
    block_start = 0
    with open(path, "rb") as f:
        BUF = 512 * 1024
        buf = b""
        buf_base = 0
        offset = 0

        def refill(frm):
            nonlocal buf, buf_base
            f.seek(frm)
            buf = f.read(BUF)
            buf_base = frm

        refill(0)
        while offset < size:
            pos = offset - buf_base
            if pos + 4 > len(buf):
                refill(offset)
                if len(buf) < 4:
                    break
                pos = 0
            if buf[pos] != 0xEA:
                offset += 1
                continue
            rtype = buf[pos + 1]
            rlen = (buf[pos + 2] << 8) | buf[pos + 3]
            if rlen < 4:
                offset += 1
                continue
            if offset + rlen > size:
                break  # truncated at EOF
            if rtype == 0x4C and rlen >= 28:
                if pos + 4 + 22 <= len(buf):
                    bdy = buf[pos + 4:pos + 4 + 22]
                else:
                    f.seek(offset + 4)
                    bdy = f.read(22)
                    refill(offset + rlen)
                flight = (bdy[20] << 8) | bdy[21]
                if 0 < flight < 65535:
                    serial = bytes(bdy[5:17]).split(b"\x00")[0].rstrip(b" ").decode(errors="replace")
                    index.append([flight, block_start, offset + rlen, serial])
                    block_start = offset + rlen
            offset += rlen
    if index:
        index[-1][2] = size  # tail belongs to the last flight's block
    return index


# ── Mount lifecycle ─────────────────────────────────────────────────────────────
def find_block_device():
    """Return the vfat/exfat partition of the ESP32's USB MSC volume, or None.

    CRITICAL: must match ONLY the AirBridge USB device — never a system disk. The host
    has its own vfat partitions (EFI/recovery on NVMe); matching the first vfat would
    mount and WRITE to a system partition. So we filter to a removable USB-transport
    disk, strongly preferring the AirBridge vendor/model, and return its vfat child."""
    try:
        out = subprocess.run(
            ["lsblk", "-J", "-o", "NAME,FSTYPE,RM,TRAN,VENDOR,MODEL,PATH"],
            capture_output=True, text=True, check=True).stdout
        data = json.loads(out)
    except (subprocess.CalledProcessError, json.JSONDecodeError):
        return None

    def is_rm(d):
        v = d.get("rm")
        return v in (True, 1, "1")

    def child_fs(disk):
        for c in disk.get("children", []):
            if c.get("fstype") in ("vfat", "exfat"):
                return c.get("path") or ("/dev/" + c.get("name", ""))
        return None

    disks = data.get("blockdevices", [])
    # First choice: the AirBridge vendor/model (unambiguous).
    for d in disks:
        ident = ((d.get("vendor") or "") + " " + (d.get("model") or "")).lower()
        if "airbridg" in ident or "sd storage" in ident:
            fs = child_fs(d)
            if fs:
                return fs
    # Fallback: a removable USB-transport disk (never nvme / non-removable system disk).
    for d in disks:
        name = d.get("name", "")
        if name.startswith("nvme") or name.startswith("mmcblk"):
            continue
        if d.get("tran") == "usb" and is_rm(d):
            fs = child_fs(d)
            if fs:
                return fs
    return None


def mount_device(timeout=120):
    """Find + mount the device's USB volume, retrying until media is ready (~90s
    presentation delay in CDC+MSC). Returns (mount_point, block_device)."""
    mnt = tempfile.mkdtemp(prefix="airbridge_dsu_")
    uid, gid = os.getuid(), os.getgid()
    deadline = time.time() + timeout
    while time.time() < deadline:
        dev = find_block_device()
        if dev:
            # Drop the kernel's stale buffer cache for the underlying disk FIRST. After a
            # USB MSC power-cycle the device re-enumerates under the SAME name (/dev/sdX)
            # but the kernel may keep STALE cached FAT/blocks from the previous instance —
            # so a mount would read a stale directory and writes would land on cache
            # blocks that never reach the live device (no WRITE10, no harvest). Flushing
            # forces reads/writes to hit the real device.
            disk = dev.rstrip("0123456789")  # /dev/sda1 -> /dev/sda
            subprocess.run(["sudo", "blockdev", "--flushbufs", disk], capture_output=True)
            r = subprocess.run(
                ["sudo", "mount", "-o",
                 f"noatime,uid={uid},gid={gid},umask=0000", dev, mnt],
                capture_output=True, text=True)
            if r.returncode == 0:
                print(f"[host_dsu] mounted {dev} -> {mnt}")
                return mnt, dev
        time.sleep(2)
    os.rmdir(mnt)
    sys.exit("[host_dsu] ERROR: could not mount device USB volume (media not ready?)")


def unmount(mnt):
    subprocess.run(["sync"])
    subprocess.run(["sudo", "umount", mnt], capture_output=True)
    try:
        os.rmdir(mnt)
    except OSError:
        pass


# ── Emit modes ──────────────────────────────────────────────────────────────────
def date_str():
    return time.strftime("%Y%m%d")


def write_metrics_and_report(mnt, filename, nbytes, metrics_src):
    mdir = os.path.join(mnt, "metrics")
    os.makedirs(mdir, exist_ok=True)
    names = [("dsuMetric.eacmf", METRIC_SIZE)] + \
            [(f"dsuMetric.{n}.eacmf", 0 if n == 2 else METRIC_SIZE)
             for n in (1, 2, 3, 5, 6, 7, 8)] + \
            [("dsuUsage.eacuf", USAGE_SIZE)]
    for name, fallback in names:
        dst = os.path.join(mdir, name)
        src = os.path.join(metrics_src, name) if metrics_src else None
        if src and os.path.exists(src):
            with open(src, "rb") as sf, open(dst, "wb") as df:
                df.write(sf.read())
        else:
            with open(dst, "wb") as df:
                df.write(b"\x00" * fallback)
    with open(os.path.join(mnt, "downloadReport.txt"), "w") as f:
        f.write(f"Downloading FH to /mnt/memStick/flightHistory/{filename} ...\n")
        f.write(f"  Estimated download file size is ~{nbytes // 1024} KB\n")
        f.write(f"Finished download FH to /mnt/memStick/flightHistory/{filename}\n")


def emit_synthetic(mnt, serial, max_flight, size_kb, first_flight, metrics_src, meta,
                   ignore_cookie=False):
    """Cookie-aware synthesis: emit ${serial}_${max_flight} only if the device
    hasn't already harvested it. first_flight defaults to 1 (matches write_dsu_file:
    Lambda consecutive-hwm logic), or set explicitly for the delta test.
    ignore_cookie forces emission even when caught up — used to feed the firmware a
    flight the manifest already covers, to exercise its manifest-skip path."""
    cookie = read_cookie_flight(mnt)
    if cookie >= max_flight and not ignore_cookie:
        print(f"[host_dsu] caught up — cookie={cookie} >= max_flight={max_flight}; nothing to emit")
        return 0
    ff = first_flight if first_flight is not None else 1
    fhdir = os.path.join(mnt, "flightHistory")
    os.makedirs(fhdir, exist_ok=True)
    fname = f"{serial}_{max_flight:05d}_{date_str()}.eaofh"
    fpath = os.path.join(fhdir, fname)
    with open(fpath, "wb") as f:
        # First-flight record + 0xEA framing byte (satisfies firstRecordFromLog),
        # then random body, then the last-flight (max_flight) record.
        f.write(make_record(serial, ff))
        f.write(b"\xEA")
        f.write(os.urandom(size_kb * 1024))
        f.write(make_record(serial, max_flight))
    nbytes = os.path.getsize(fpath)
    if meta:
        with open(fpath + ".meta", "w") as mf:
            mf.write(f"{ff}:{max_flight}\n")
    write_metrics_and_report(mnt, fname, nbytes, metrics_src)
    print(f"[host_dsu] emitted flightHistory/{fname} "
          f"(first={ff} last={max_flight} cookie={cookie} {nbytes} bytes)")
    return nbytes


def emit_slice(mnt, serial, max_flight, fixture, metrics_src):
    """Faithful sim_dsu.h port: copy the byte slice for flights (cookie, max_flight]
    from a real fixture, rewriting the embedded serial to `serial`."""
    cookie = read_cookie_flight(mnt)
    index = build_flight_index(fixture)
    if not index:
        sys.exit(f"[host_dsu] ERROR: empty flight index from {fixture}")
    print(f"[host_dsu] index: {len(index)} flights "
          f"{index[0][0]}-{index[-1][0]} from {fixture}")
    start = next((e for e in index if e[0] > cookie), None)
    if start is None:
        print(f"[host_dsu] caught up — no flights after {cookie}")
        return 0
    in_range = [e for e in index if e[0] > cookie and e[0] <= max_flight]
    if not in_range:
        print(f"[host_dsu] no flights in range ({cookie}, {max_flight}]")
        return 0
    slice_start = in_range[0][1]
    slice_end = in_range[-1][2]
    last_flight = in_range[-1][0]
    fhdir = os.path.join(mnt, "flightHistory")
    os.makedirs(fhdir, exist_ok=True)
    fname = f"{serial}_{last_flight:05d}_{date_str()}.eaofh"
    fpath = os.path.join(fhdir, fname)
    sb = serial.encode()[:12].ljust(12, b"\x00")
    with open(fixture, "rb") as sf, open(fpath, "wb") as df:
        sf.seek(slice_start)
        rem = slice_end - slice_start
        data = bytearray(sf.read(rem))
    # Rewrite serial in every 0x4C record within the slice (body[5:17]).
    off = 0
    while off + 4 <= len(data):
        if data[off] == 0xEA and data[off + 1] == 0x4C:
            rlen = (data[off + 2] << 8) | data[off + 3]
            if rlen >= 28 and off + rlen <= len(data):
                data[off + 4 + 5:off + 4 + 17] = sb
                off += rlen
                continue
        off += 1
    with open(fpath, "wb") as df:
        df.write(data)
    nbytes = len(data)
    write_metrics_and_report(mnt, fname, nbytes, metrics_src)
    print(f"[host_dsu] sliced flightHistory/{fname} "
          f"(flights {in_range[0][0]}-{last_flight} cookie={cookie} {nbytes} bytes)")
    return nbytes


def emit_partial(mnt, serial, flight, preamble_kb):
    """Write a crafted partial .eaofh: random preamble + record(flight-1) +
    record(flight) + a TRUNCATED header tail. The firmware's backward scanner
    rejects the truncated header and recovers `flight`. (Power-loss recovery test.)"""
    fhdir = os.path.join(mnt, "flightHistory")
    os.makedirs(fhdir, exist_ok=True)
    fname = f"{serial}_{flight:05d}_{date_str()}.eaofh"
    fpath = os.path.join(fhdir, fname)
    with open(fpath, "wb") as f:
        f.write(os.urandom(preamble_kb * 1024))
        f.write(make_record(serial, flight - 1))
        f.write(make_record(serial, flight))
        f.write(bytes([0xEA, 0x4C, 0x00, 0x1C, 0x00, 0x00]))  # truncated header
    print(f"[host_dsu] wrote partial flightHistory/{fname} (last full record={flight})")
    return os.path.getsize(fpath)


# ── Main ────────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Cookie-aware host DSU simulator")
    ap.add_argument("--mount", help="use an already-mounted path (no sudo mount)")
    ap.add_argument("--mount-find", action="store_true",
                    help="discover + mount the device USB volume (sudo), unmount after")
    ap.add_argument("--mount-timeout", type=int, default=120,
                    help="seconds to wait for the MSC LUN to become ready")
    ap.add_argument("--serial", default="EA500.E2ETES")
    ap.add_argument("--max-flight", type=int, help="upper-bound flight to emit")
    ap.add_argument("--size-kb", type=int, default=200, help="synth body size (emit mode)")
    ap.add_argument("--first-flight", type=int, default=None,
                    help="force first_flight (emit/delta); default 1")
    ap.add_argument("--meta", action="store_true",
                    help="also write a {first}:{last} .meta sidecar (delta test)")
    ap.add_argument("--ignore-cookie", action="store_true",
                    help="emit even if the cookie already covers --max-flight "
                         "(to exercise the firmware's manifest-skip path)")
    ap.add_argument("--fixture",
                    default="esp32/test/fixtures/EA500.000243_01077.eaofh")
    ap.add_argument("--metrics-src", default="esp32/test/fixtures/metrics")
    ap.add_argument("--mode", choices=["emit", "slice"], default="emit")
    ap.add_argument("--plant-cookie", type=int, metavar="F",
                    help="write a flight-F cookie and exit")
    ap.add_argument("--read-cookie", action="store_true",
                    help="print the current cookie flight and exit")
    ap.add_argument("--partial", type=int, metavar="F",
                    help="write a crafted partial file recovering flight F, and exit")
    ap.add_argument("--partial-preamble-kb", type=int, default=50)
    args = ap.parse_args()

    if args.mount:
        mnt, owns = args.mount, False
    elif args.mount_find:
        mnt, _dev = mount_device(args.mount_timeout)
        owns = True
    else:
        ap.error("one of --mount or --mount-find is required")

    try:
        if args.read_cookie:
            print(read_cookie_flight(mnt))
            return
        if args.plant_cookie is not None:
            with open(os.path.join(mnt, "dsuCookie.easdf"), "wb") as f:
                f.write(build_cookie(args.serial, args.plant_cookie))
            print(f"[host_dsu] planted cookie {args.serial} flight={args.plant_cookie}")
            return
        if args.partial is not None:
            emit_partial(mnt, args.serial, args.partial, args.partial_preamble_kb)
            return
        if args.max_flight is None:
            ap.error("--max-flight is required for emit/slice modes")
        if args.mode == "slice":
            emit_slice(mnt, args.serial, args.max_flight, args.fixture, args.metrics_src)
        else:
            emit_synthetic(mnt, args.serial, args.max_flight, args.size_kb,
                           args.first_flight, args.metrics_src, args.meta,
                           args.ignore_cookie)
    finally:
        if owns:
            unmount(mnt)


if __name__ == "__main__":
    main()

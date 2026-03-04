"""
wifi_manager.py – WiFi status queries and file upload over wlan0.

Uses NetworkManager (nmcli) and /proc/net/wireless — no iwconfig needed.
"""

import socket
import subprocess
import ftplib
import os
import time
import requests


def _run(cmd):
    return subprocess.run(cmd, capture_output=True, text=True)


def is_connected() -> bool:
    """Return True if wlan0 can reach the internet (socket connect to 8.8.8.8:53)."""
    try:
        sock = socket.create_connection(("8.8.8.8", 53), timeout=3)
        sock.close()
        return True
    except OSError:
        return False


def get_wifi_info():
    """
    Return (rssi_dbm, ssid) for the currently associated network.
    rssi_dbm is an int (dBm) or None if not associated.  ssid is a str or "".
    """
    ssid = ""
    rssi = None

    # SSID via nmcli (available even without root)
    res = _run(["nmcli", "-t", "-f", "active,ssid", "dev", "wifi"])
    for line in res.stdout.splitlines():
        if line.startswith("yes:"):
            ssid = line[4:].strip()
            break

    # RSSI from /proc/net/wireless (kernel-reported, no root needed)
    # Format:  wlan0: 0000   70.  -25.  -256.  ...
    #          fields: iface  status  link  level  noise  ...
    try:
        with open("/proc/net/wireless") as f:
            for line in f:
                if "wlan0:" in line:
                    parts = line.split()
                    if len(parts) >= 4:
                        rssi = int(parts[3].rstrip("."))
                    break
    except Exception:
        pass

    return rssi, ssid


def rssi_to_csq(dbm) -> int:
    """
    Map RSSI (dBm) to CSQ (0–31, 99=unknown).
    -30 dBm → 31 (max), -90 dBm → 0 (min), None → 99.
    """
    if dbm is None:
        return 99
    csq = int((dbm + 90) * 31 / 60)
    return max(0, min(31, csq))


def add_network(ssid: str, password: str) -> None:
    """
    Connect to a new WiFi network via NetworkManager.
    Blocks until NM attempts the connection (up to 30 s).
    """
    cmd = ["sudo", "nmcli", "dev", "wifi", "connect", ssid]
    if password:
        cmd += ["password", password]
    _run(cmd)


def upload_ftp(server, port, username, password, filepath,
               remote_path="/", max_retries=3, retry_delay=5,
               progress_callback=None):
    """
    Upload a file via FTP using ftplib over wlan0.
    Returns (success: bool, message: str).
    """
    if not os.path.exists(filepath):
        return False, f"File not found: {filepath}"

    filesize = os.path.getsize(filepath)
    filename = os.path.basename(filepath)
    remote   = remote_path.rstrip("/") + "/" + filename

    print(f"WiFi FTP upload: {filename} ({filesize} bytes) → {server}")

    for attempt in range(max_retries):
        try:
            ftp = ftplib.FTP(timeout=30)
            ftp.connect(server, port)
            ftp.login(username, password)
            ftp.set_pasv(True)
            ftp.sendcmd("TYPE I")

            offset = 0
            try:
                server_size = ftp.size(remote) or 0
                if server_size > 0 and server_size < filesize:
                    offset = server_size
                    print(f"  Resuming from byte {offset}")
                elif server_size >= filesize:
                    ftp.quit()
                    return True, "Already complete"
            except ftplib.error_perm:
                offset = 0

            bytes_sent = [offset]

            def _progress(data):
                bytes_sent[0] += len(data)
                if progress_callback:
                    progress_callback(bytes_sent[0], filesize)

            with open(filepath, "rb") as f:
                if offset > 0:
                    f.seek(offset)
                    ftp.sendcmd(f"REST {offset}")
                ftp.storbinary(f"STOR {remote}", f,
                               blocksize=65536, callback=_progress)

            ftp.quit()
            print(f"Upload complete: {filename}")
            return True, "Upload successful"

        except (ftplib.Error, OSError, TimeoutError) as exc:
            print(f"FTP attempt {attempt+1}/{max_retries} failed: {exc}")
            if attempt < max_retries - 1:
                time.sleep(retry_delay)

    return False, f"Upload failed after {max_retries} attempts"


def upload_http(url, filepath, chunk_size=65536,
                progress_callback=None, max_retries=3):
    """
    Upload a file via chunked HTTP POST over wlan0.
    Returns (success: bool, message: str).
    """
    if not os.path.exists(filepath):
        return False, f"File not found: {filepath}"

    filesize = os.path.getsize(filepath)
    filename = os.path.basename(filepath)

    print(f"WiFi HTTP upload: {filename} ({filesize} bytes) → {url}")

    with open(filepath, "rb") as f:
        offset = 0
        while offset < filesize:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            chunk_len = len(chunk)

            sent = False
            for attempt in range(max_retries):
                chunk_url = f"{url}?offset={offset}&total={filesize}"
                try:
                    resp = requests.post(
                        chunk_url, data=chunk,
                        headers={"Content-Type": "application/octet-stream"},
                        timeout=120,
                    )
                    if resp.status_code in (200, 201, 204):
                        sent = True
                        break
                    print(f"  HTTP {resp.status_code} (attempt {attempt+1})")
                except requests.RequestException as exc:
                    print(f"  HTTP error (attempt {attempt+1}): {exc}")
                if attempt < max_retries - 1:
                    time.sleep(2)

            if not sent:
                return False, f"HTTP chunk failed at offset {offset}"

            offset += chunk_len
            if progress_callback:
                progress_callback(offset, filesize)

    return True, "Upload complete"

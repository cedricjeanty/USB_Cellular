"""
captive_portal.py – WiFi AP with captive portal for credential entry.

Uses NetworkManager (nmcli) to create the hotspot — no hostapd/dnsmasq install
needed.  NM's built-in dnsmasq reads /etc/NetworkManager/dnsmasq-shared.d/ so
we drop a one-line DNS redirect there to trigger iOS/Android captive portal.

Usage:
    portal = CaptivePortal(ssid="AirBridge", channel=6)
    portal.start()
    creds = portal.wait_for_credentials(timeout=300)
    if creds:
        ssid, password = creds
        add_network(ssid, password)
    portal.stop()
"""

import os
import subprocess
import threading
import time
import logging
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs

log = logging.getLogger('airbridge')

AP_IP      = "192.168.4.1"
AP_SUBNET  = "192.168.4.1/24"
NM_CON     = "airbridge-ap"
DNS_CONF   = "/etc/NetworkManager/dnsmasq-shared.d/airbridge-captive.conf"

_PORTAL_HTML = """\
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AirBridge WiFi Setup</title>
<style>
body { font-family: sans-serif; max-width: 400px; margin: 40px auto; padding: 0 20px; }
h2   { color: #333; }
p    { color: #555; font-size: 14px; }
input  { display: block; width: 100%; padding: 10px; margin: 8px 0;
         box-sizing: border-box; font-size: 16px; border: 1px solid #ccc;
         border-radius: 4px; }
button { width: 100%; padding: 12px; background: #0078d4; color: white;
         border: none; font-size: 16px; border-radius: 4px; cursor: pointer; }
button:hover { background: #005a9e; }
</style>
</head>
<body>
<h2>AirBridge WiFi Setup</h2>
<p>Connect this device to your WiFi network for file uploads.</p>
<form method="POST" action="/configure">
  <input name="ssid" placeholder="WiFi Network Name" required autocorrect="off" autocapitalize="off">
  <input name="password" type="password" placeholder="Password (leave blank if open)">
  <button type="submit">Connect</button>
</form>
<hr>
<p style="font-size:12px;color:#888">SSH: cedric@{ip} &nbsp;|&nbsp; HTTP: http://{ip}</p>
</body>
</html>
""".replace("{ip}", AP_IP)

_SUCCESS_HTML = """\
<!DOCTYPE html>
<html>
<head><meta charset="utf-8"><title>AirBridge</title></head>
<body>
<h2>Connecting&hellip;</h2>
<p>AirBridge will now connect to your WiFi. You can close this page.</p>
</body>
</html>
"""


def _run(cmd, check=False):
    return subprocess.run(cmd, capture_output=True, text=True, check=check)


class _PortalHandler(BaseHTTPRequestHandler):
    portal = None   # injected by CaptivePortal.start()

    def log_message(self, fmt, *args):
        log.debug("portal HTTP: " + fmt % args)

    def _html(self, code, body):
        enc = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(enc)))
        self.end_headers()
        self.wfile.write(enc)

    def do_GET(self):
        # iOS / Android captive-portal probes get the form → triggers popup
        self._html(200, _PORTAL_HTML)

    def do_POST(self):
        if not self.path.startswith("/configure"):
            self._html(404, "Not found")
            return
        n   = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(n).decode(errors="replace")
        p   = parse_qs(raw)
        ssid = p.get("ssid", [""])[0].strip()
        pwd  = p.get("password", [""])[0]
        if ssid and self.portal is not None:
            self.portal.credentials = (ssid, pwd)
            log.info(f"Portal: credentials received for SSID '{ssid}'")
            self._html(200, _SUCCESS_HTML)
        else:
            self._html(400, "Missing SSID")


class CaptivePortal:
    """
    NM-backed WiFi AP with a captive portal HTTP server.
    Starts an open 'AirBridge' network; all DNS resolves to AP_IP so
    phones show the 'Sign in to network' prompt.
    """

    def __init__(self, ssid="AirBridge", channel=6):
        self.ssid        = ssid
        self.channel     = channel
        self.credentials = None
        self._httpd      = None
        self._thread     = None

    # ── AP management ─────────────────────────────────────────────────────────

    def start(self) -> None:
        log.info(f"Starting captive portal AP: SSID={self.ssid!r} on {AP_IP}")

        # 1. Drop DNS redirect config so NM's dnsmasq sends all queries → AP_IP
        os.makedirs(os.path.dirname(DNS_CONF), exist_ok=True)
        try:
            with open(DNS_CONF, "w") as f:
                f.write(f"address=/#/{AP_IP}\n")
        except PermissionError:
            _run(["sudo", "sh", "-c",
                  f'echo "address=/#/{AP_IP}" > {DNS_CONF}'])

        # 2. Remove stale connection if present
        _run(["sudo", "nmcli", "con", "delete", NM_CON])

        # 3. Create NM AP connection
        r = _run([
            "sudo", "nmcli", "con", "add",
            "type", "wifi", "ifname", "wlan0",
            "con-name", NM_CON,
            "ssid", self.ssid,
            "mode", "ap",
            "ipv4.method", "shared",
            "ipv4.addresses", AP_SUBNET,
            "wifi.band", "bg",
            "wifi.channel", str(self.channel),
        ])
        if r.returncode != 0:
            log.error(f"NM AP add failed: {r.stderr.strip()}")
            return

        # Bring up AP — fire and don't wait (SSH drops when Pi leaves client mode)
        subprocess.Popen(
            ["sudo", "nmcli", "con", "up", NM_CON],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        time.sleep(3)   # give NM time to activate and start dnsmasq

        # 4. Start HTTP server
        _PortalHandler.portal = self
        try:
            self._httpd = HTTPServer(("0.0.0.0", 80), _PortalHandler)
        except PermissionError:
            log.warning("Port 80 unavailable (no root?), using 8080")
            self._httpd = HTTPServer(("0.0.0.0", 8080), _PortalHandler)

        self._thread = threading.Thread(
            target=self._httpd.serve_forever,
            name="portal-http", daemon=True,
        )
        self._thread.start()
        log.info(f"Portal HTTP server running at http://{AP_IP}")

    def stop(self) -> None:
        log.info("Stopping captive portal")

        if self._httpd:
            self._httpd.shutdown()
            self._httpd = None

        # Remove DNS redirect config
        try:
            os.remove(DNS_CONF)
        except FileNotFoundError:
            pass
        except PermissionError:
            _run(["sudo", "rm", "-f", DNS_CONF])

        # Tear down AP
        _run(["sudo", "nmcli", "con", "down", NM_CON])
        _run(["sudo", "nmcli", "con", "delete", NM_CON])

        # Explicitly reconnect to the first auto-connect WiFi connection
        res = _run(["sudo", "nmcli", "-t", "-f", "name,type,connection.autoconnect",
                    "con", "show"])
        for line in res.stdout.splitlines():
            parts = line.split(":")
            if len(parts) >= 2 and "wireless" in parts[1].lower() and parts[0] != NM_CON:
                _run(["sudo", "nmcli", "con", "up", parts[0]])
                break
        else:
            # Fallback: ask NM to auto-connect on wlan0
            _run(["sudo", "nmcli", "dev", "connect", "wlan0"])

        # Wait for NM to reconnect to a saved network (up to 30 s)
        deadline = time.time() + 30
        while time.time() < deadline:
            res = _run(["nmcli", "-t", "-f", "active", "dev", "wifi"])
            if "yes" in res.stdout:
                log.info("wlan0 reconnected after portal stop")
                break
            time.sleep(2)

    def wait_for_credentials(self, timeout: int = 300):
        """Block until form submitted or timeout. Returns (ssid, pwd) or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.credentials is not None:
                return self.credentials
            time.sleep(1)
        return None

#!/usr/bin/env python3
"""
USB Cellular Airbridge - Status Web Server
Lightweight Flask app to display status and logs.
"""

import os
import subprocess
import json
from datetime import datetime
from flask import Flask, render_template_string, jsonify

app = Flask(__name__)

# HTML template with auto-refresh
HTML_TEMPLATE = """
<!DOCTYPE html>
<html>
<head>
    <title>Airbridge Status</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, monospace;
            background: #1a1a2e;
            color: #eee;
            margin: 0;
            padding: 20px;
        }
        h1 { color: #00d4ff; margin-bottom: 5px; }
        .subtitle { color: #666; margin-bottom: 20px; }
        .card {
            background: #16213e;
            border-radius: 8px;
            padding: 15px;
            margin-bottom: 15px;
        }
        .card h2 {
            margin: 0 0 10px 0;
            font-size: 14px;
            color: #888;
            text-transform: uppercase;
        }
        .status-grid {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
            gap: 10px;
        }
        .status-item {
            background: #0f3460;
            padding: 12px;
            border-radius: 6px;
        }
        .status-item .label {
            font-size: 11px;
            color: #888;
            text-transform: uppercase;
        }
        .status-item .value {
            font-size: 18px;
            font-weight: bold;
            margin-top: 4px;
        }
        .status-ok { color: #00ff88; }
        .status-warn { color: #ffaa00; }
        .status-err { color: #ff4444; }
        .logs {
            background: #0a0a0a;
            padding: 10px;
            border-radius: 6px;
            font-family: monospace;
            font-size: 11px;
            max-height: 400px;
            overflow-y: auto;
            white-space: pre-wrap;
            word-break: break-all;
        }
        .logs .info { color: #00d4ff; }
        .logs .warn { color: #ffaa00; }
        .logs .err { color: #ff4444; }
        .refresh-info {
            text-align: center;
            color: #444;
            font-size: 11px;
            margin-top: 10px;
        }
        .btn {
            background: #0f3460;
            color: #00d4ff;
            border: 1px solid #00d4ff;
            padding: 8px 16px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 12px;
            margin-right: 8px;
        }
        .btn:hover { background: #1a4a80; }
    </style>
</head>
<body>
    <h1>Airbridge Status</h1>
    <p class="subtitle">USB Cellular Data Bridge</p>

    <div class="card">
        <h2>System Status</h2>
        <div class="status-grid">
            <div class="status-item">
                <div class="label">Service</div>
                <div class="value {{ 'status-ok' if status.service_active else 'status-err' }}">
                    {{ 'Running' if status.service_active else 'Stopped' }}
                </div>
            </div>
            <div class="status-item">
                <div class="label">USB Gadget</div>
                <div class="value {{ 'status-ok' if status.usb_state == 'configured' else 'status-warn' }}">
                    {{ status.usb_state | capitalize }}
                </div>
            </div>
            <div class="status-item">
                <div class="label">Uptime</div>
                <div class="value">{{ status.uptime }}</div>
            </div>
            <div class="status-item">
                <div class="label">CPU Temp</div>
                <div class="value {{ 'status-warn' if status.cpu_temp > 70 else '' }}">
                    {{ status.cpu_temp }}°C
                </div>
            </div>
        </div>
    </div>

    <div class="card">
        <h2>USB Disk</h2>
        <div class="status-grid">
            <div class="status-item">
                <div class="label">Partition Size</div>
                <div class="value">{{ status.usb_disk.size }}</div>
            </div>
            <div class="status-item">
                <div class="label">Data Written</div>
                <div class="value" style="font-size: 14px;">{{ "%.1f"|format(status.usb_disk.write_bytes / 1024 / 1024) }} MB</div>
            </div>
            <div class="status-item">
                <div class="label">Activity</div>
                <div class="value {{ 'status-ok' if 'Writing' in status.usb_disk.activity else '' }}" style="font-size: 14px;">
                    {{ status.usb_disk.activity }}
                </div>
            </div>
        </div>
    </div>

    <div class="card">
        <h2>Cellular</h2>
        <div class="status-grid">
            <div class="status-item">
                <div class="label">Signal (CSQ)</div>
                <div class="value {{ 'status-ok' if status.signal > 15 else 'status-warn' if status.signal > 5 else 'status-err' }}">
                    {{ status.signal if status.signal != 99 else 'N/A' }}
                </div>
            </div>
            <div class="status-item">
                <div class="label">Network</div>
                <div class="value {{ 'status-ok' if status.registered else 'status-err' }}">
                    {{ 'Registered' if status.registered else 'Not registered' }}
                </div>
            </div>
            <div class="status-item">
                <div class="label">IP Address</div>
                <div class="value" style="font-size: 14px;">{{ status.ip or 'None' }}</div>
            </div>
        </div>
    </div>

    <div class="card">
        <h2>Upload Status</h2>
        <div class="status-grid">
            <div class="status-item">
                <div class="label">Pending Files</div>
                <div class="value {{ 'status-warn' if status.pending_count > 0 else '' }}">
                    {{ status.pending_count if status.pending_count >= 0 else '?' }}
                </div>
            </div>
            <div class="status-item">
                <div class="label">Current Upload</div>
                <div class="value" style="font-size: 14px;">
                    {% if status.upload_progress %}
                        {{ status.upload_progress.pct }}%
                    {% else %}
                        Idle
                    {% endif %}
                </div>
            </div>
        </div>
        {% if status.upload_progress %}
        <div style="margin-top: 10px; background: #0a0a0a; border-radius: 4px; padding: 2px;">
            <div style="background: #00d4ff; height: 20px; border-radius: 3px; width: {{ status.upload_progress.pct }}%;"></div>
        </div>
        <div style="font-size: 11px; color: #666; margin-top: 5px;">
            {{ status.upload_progress.file }} - {{ status.upload_progress.bytes_sent }} / {{ status.upload_progress.filesize }} bytes
        </div>
        {% endif %}
    </div>

    <div class="card">
        <h2>Recent Logs</h2>
        <div style="margin-bottom: 10px;">
            <button class="btn" onclick="location.reload()">Refresh</button>
            <button class="btn" onclick="fetchLogs(50)">Last 50</button>
            <button class="btn" onclick="fetchLogs(200)">Last 200</button>
        </div>
        <div class="logs" id="logs">{{ logs | safe }}</div>
    </div>

    <p class="refresh-info">Auto-refresh every 10 seconds | Last update: {{ status.timestamp }}</p>

    <script>
        // Auto-refresh page every 1 second
        setTimeout(() => location.reload(), 1000);

        function fetchLogs(n) {
            fetch('/api/logs?n=' + n)
                .then(r => r.json())
                .then(data => {
                    document.getElementById('logs').innerHTML = data.logs;
                });
        }

        // Scroll logs to bottom
        const logsDiv = document.getElementById('logs');
        logsDiv.scrollTop = logsDiv.scrollHeight;
    </script>
</body>
</html>
"""

def get_service_status():
    """Check if airbridge service is active."""
    result = subprocess.run(
        ["systemctl", "is-active", "airbridge.service"],
        capture_output=True, text=True
    )
    return result.stdout.strip() == "active"

def get_usb_state():
    """Get USB gadget state."""
    try:
        udc_dirs = os.listdir("/sys/class/udc/")
        if udc_dirs:
            with open(f"/sys/class/udc/{udc_dirs[0]}/state", 'r') as f:
                return f.read().strip()
    except:
        pass
    return "unknown"

def get_uptime():
    """Get system uptime."""
    try:
        with open('/proc/uptime', 'r') as f:
            uptime_seconds = float(f.readline().split()[0])
        hours = int(uptime_seconds // 3600)
        minutes = int((uptime_seconds % 3600) // 60)
        if hours > 24:
            days = hours // 24
            hours = hours % 24
            return f"{days}d {hours}h"
        return f"{hours}h {minutes}m"
    except:
        return "N/A"

def get_cpu_temp():
    """Get CPU temperature."""
    try:
        with open('/sys/class/thermal/thermal_zone0/temp', 'r') as f:
            temp = int(f.read().strip()) / 1000
        return round(temp, 1)
    except:
        return 0

def get_cellular_status():
    """Get cellular modem status via AT commands."""
    import serial
    import yaml

    signal = 99
    registered = False
    ip = None

    try:
        with open('/home/cedric/config.yaml', 'r') as f:
            cfg = yaml.safe_load(f)

        ser = serial.Serial(cfg['serial']['port'], cfg['serial']['baudrate'], timeout=1)

        # Check signal
        ser.write(b"AT+CSQ\r\n")
        import time
        time.sleep(0.5)
        resp = ser.read(ser.in_waiting).decode(errors='ignore')
        if "+CSQ:" in resp:
            try:
                signal = int(resp.split(":")[1].split(",")[0].strip())
            except:
                pass

        # Check registration
        ser.write(b"AT+CREG?\r\n")
        time.sleep(0.5)
        resp = ser.read(ser.in_waiting).decode(errors='ignore')
        if "+CREG:" in resp:
            registered = ",1" in resp or ",5" in resp

        # Check IP
        ser.write(b"AT+SAPBR=2,1\r\n")
        time.sleep(0.5)
        resp = ser.read(ser.in_waiting).decode(errors='ignore')
        if "+SAPBR:" in resp and '"' in resp:
            try:
                ip = resp.split('"')[1]
                if ip == "0.0.0.0":
                    ip = None
            except:
                pass

        ser.close()
    except Exception as e:
        pass

    return signal, registered, ip

# Track previous disk stats for activity detection
_last_disk_stats = {'writes': 0, 'timestamp': 0}

def get_usb_disk_info():
    """Get USB disk size and activity info."""
    import yaml
    import time

    global _last_disk_stats

    try:
        with open('/home/cedric/config.yaml', 'r') as f:
            cfg = yaml.safe_load(f)

        disk_path = cfg.get('virtual_disk_path', '/dev/mmcblk0p3')

        # Extract partition name (e.g., mmcblk0p3)
        if disk_path.startswith('/dev/'):
            part_name = disk_path.replace('/dev/', '')
            # Handle both /dev/mmcblk0p3 and /dev/sda1 formats
            if 'mmcblk' in part_name:
                disk_name = part_name.rstrip('0123456789').rstrip('p')  # mmcblk0
            else:
                disk_name = part_name.rstrip('0123456789')  # sda

            # Get partition size (in 512-byte sectors)
            size_path = f"/sys/block/{disk_name}/{part_name}/size"
            if os.path.exists(size_path):
                with open(size_path, 'r') as f:
                    sectors = int(f.read().strip())
                size_bytes = sectors * 512

                # Get I/O stats
                stat_path = f"/sys/block/{disk_name}/{part_name}/stat"
                write_sectors = 0
                if os.path.exists(stat_path):
                    with open(stat_path, 'r') as f:
                        stats = f.read().split()
                        # Format: reads_completed reads_merged read_sectors read_ms
                        #         writes_completed writes_merged write_sectors write_ms ...
                        if len(stats) >= 7:
                            write_sectors = int(stats[6])  # write_sectors

                # Calculate write activity
                now = time.time()
                write_bytes = write_sectors * 512
                activity = "Idle"

                if _last_disk_stats['timestamp'] > 0:
                    time_diff = now - _last_disk_stats['timestamp']
                    if time_diff > 0:
                        bytes_diff = write_bytes - _last_disk_stats['writes']
                        if bytes_diff > 0:
                            rate = bytes_diff / time_diff
                            if rate > 1024 * 1024:
                                activity = f"Writing {rate/1024/1024:.1f} MB/s"
                            elif rate > 1024:
                                activity = f"Writing {rate/1024:.1f} KB/s"
                            elif rate > 0:
                                activity = f"Writing {rate:.0f} B/s"

                _last_disk_stats['writes'] = write_bytes
                _last_disk_stats['timestamp'] = now

                # Format size
                if size_bytes >= 1024 * 1024 * 1024:
                    size_str = f"{size_bytes / 1024 / 1024 / 1024:.1f} GB"
                else:
                    size_str = f"{size_bytes / 1024 / 1024:.0f} MB"

                return {
                    'size': size_str,
                    'size_bytes': size_bytes,
                    'write_bytes': write_bytes,
                    'activity': activity
                }
        else:
            # File-backed image
            if os.path.exists(disk_path):
                size_bytes = os.path.getsize(disk_path)
                mtime = os.path.getmtime(disk_path)
                size_str = f"{size_bytes / 1024 / 1024:.0f} MB"
                return {
                    'size': size_str,
                    'size_bytes': size_bytes,
                    'write_bytes': 0,
                    'activity': 'File-backed'
                }
    except Exception as e:
        pass

    return {'size': 'N/A', 'size_bytes': 0, 'write_bytes': 0, 'activity': 'Unknown'}

def get_pending_uploads():
    """Get count of pending upload files."""
    # Outbox is on USB partition - may not be mounted
    outbox = "/mnt/usb_logs/.airbridge_outbox"
    try:
        if os.path.exists(outbox):
            files = [f for f in os.listdir(outbox) if f.endswith(('.zip', '.gz'))]
            return len(files)
    except:
        pass
    # Can't check when disk is in gadget mode (not mounted)
    return -1  # -1 indicates unknown

def get_upload_progress():
    """Get current upload progress if any."""
    progress_file = "/tmp/airbridge_upload_progress.json"
    try:
        if os.path.exists(progress_file):
            with open(progress_file, 'r') as f:
                data = json.load(f)
            filepath = data.get('filepath', '')
            bytes_sent = data.get('bytes_sent', 0)
            # Try to get file size
            if os.path.exists(filepath):
                filesize = os.path.getsize(filepath)
                pct = int(100 * bytes_sent / filesize) if filesize > 0 else 0
                return {
                    'file': os.path.basename(filepath),
                    'bytes_sent': bytes_sent,
                    'filesize': filesize,
                    'pct': pct
                }
    except:
        pass
    return None

def get_logs(n=50):
    """Get recent logs from journalctl."""
    try:
        result = subprocess.run(
            ["journalctl", "-u", "airbridge.service", "-n", str(n), "--no-pager", "-o", "short"],
            capture_output=True, text=True
        )
        lines = result.stdout.strip().split('\n')
        # Colorize log levels
        colored = []
        for line in lines:
            if 'ERROR' in line or 'error' in line.lower():
                colored.append(f'<span class="err">{line}</span>')
            elif 'WARNING' in line or 'warn' in line.lower():
                colored.append(f'<span class="warn">{line}</span>')
            elif 'INFO' in line:
                colored.append(f'<span class="info">{line}</span>')
            else:
                colored.append(line)
        return '\n'.join(colored)
    except Exception as e:
        return f"Error reading logs: {e}"

@app.route('/')
def index():
    """Main status page."""
    signal, registered, ip = get_cellular_status()

    status = {
        'service_active': get_service_status(),
        'usb_state': get_usb_state(),
        'usb_disk': get_usb_disk_info(),
        'uptime': get_uptime(),
        'cpu_temp': get_cpu_temp(),
        'signal': signal,
        'registered': registered,
        'ip': ip,
        'pending_count': get_pending_uploads(),
        'upload_progress': get_upload_progress(),
        'timestamp': datetime.now().strftime('%H:%M:%S')
    }

    logs = get_logs(50)

    return render_template_string(HTML_TEMPLATE, status=status, logs=logs)

@app.route('/api/status')
def api_status():
    """JSON API for status."""
    signal, registered, ip = get_cellular_status()

    return jsonify({
        'service_active': get_service_status(),
        'usb_state': get_usb_state(),
        'usb_disk': get_usb_disk_info(),
        'uptime': get_uptime(),
        'cpu_temp': get_cpu_temp(),
        'signal': signal,
        'registered': registered,
        'ip': ip,
        'pending_count': get_pending_uploads(),
        'upload_progress': get_upload_progress(),
        'timestamp': datetime.now().isoformat()
    })

@app.route('/api/logs')
def api_logs():
    """JSON API for logs."""
    from flask import request
    n = request.args.get('n', 50, type=int)
    return jsonify({'logs': get_logs(n)})

if __name__ == '__main__':
    # Run on all interfaces, port 8080
    app.run(host='0.0.0.0', port=8080, debug=False)

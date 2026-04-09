#!/usr/bin/env python3
"""
Minimal HTTP receiver for Airbridge uploads.

Run on any internet-accessible Linux machine:
    pip install flask
    python3 scripts/http_receiver.py --upload-dir /var/airbridge/uploads

The modem posts chunks as:
    POST /upload/<filename>?offset=N&total=T
with the raw bytes as the body.  Chunks are appended in order; the final
chunk triggers a "complete" log line.

In config.yaml, set:
    http_upload:
        url_base: "http://<your-server-ip>:8080/upload"

Then set upload_method: http in config.yaml to switch from FTP.
"""

import os
import argparse
import logging
from flask import Flask, request, abort

log = logging.getLogger("receiver")
logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")

app = Flask(__name__)
UPLOAD_DIR = "/tmp/airbridge_uploads"


@app.route("/upload/<filename>", methods=["POST", "PUT"])
def receive_chunk(filename):
    try:
        offset = int(request.args.get("offset", 0))
        total  = int(request.args.get("total",  0))
    except ValueError:
        abort(400, "Bad offset/total params")

    data = request.get_data()
    if not data:
        abort(400, "Empty body")

    os.makedirs(UPLOAD_DIR, exist_ok=True)
    dest = os.path.join(UPLOAD_DIR, filename)

    # Write or append depending on offset
    mode = "r+b" if offset > 0 and os.path.exists(dest) else "wb"
    with open(dest, mode) as f:
        if offset > 0:
            f.seek(offset)
        f.write(data)

    received = offset + len(data)
    log.info(f"{filename}: {received}/{total} bytes ({100*received//max(total,1)}%)")

    if total > 0 and received >= total:
        log.info(f"✓ {filename} complete ({total} bytes) → {dest}")

    return "OK", 200


@app.route("/health")
def health():
    return "OK", 200


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--upload-dir", default="/tmp/airbridge_uploads",
                        help="Directory to store uploaded files")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()

    UPLOAD_DIR = args.upload_dir
    os.makedirs(UPLOAD_DIR, exist_ok=True)
    log.info(f"Listening on 0.0.0.0:{args.port}, saving to {UPLOAD_DIR}")
    app.run(host="0.0.0.0", port=args.port)

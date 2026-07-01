"""
Lambda function for AirBridge S3 pre-signed URL generation.

Routes:
  GET  /presign             — Start upload or get a per-part pre-signed URL
  POST /complete            — Finalize a multipart upload
  GET  /aircraft/manifest   — Return per-DSU manifest (hwm + file list)
  POST /aircraft/manifest   — Update manifest with a newly uploaded file
"""

import base64
import json
import math
import os
import re
from datetime import datetime, timezone

import boto3
from botocore.config import Config

REGION = os.environ.get("AWS_REGION", "us-west-2")
s3 = boto3.client(
    "s3",
    region_name=REGION,
    config=Config(signature_version="s3v4", s3={"addressing_style": "virtual"}),
    endpoint_url=f"https://s3.{REGION}.amazonaws.com",
)
BUCKET = os.environ["BUCKET"]
BUCKET_TEST = os.environ.get("BUCKET_TEST", BUCKET)
# Emulator devices use "EMU_" prefix; test serials use "EA500.E2E" prefix.
# Both are routed to the test bucket so test runs never touch production data.
CHUNK = 5 * 1024 * 1024  # 5 MB — S3 multipart minimum for non-final parts
URL_EXPIRY = 3600  # 1 hour


def _pick_bucket(event):
    """Return prod or test bucket based on device ID / DSU serial in the request."""
    params = event.get("queryStringParameters") or {}
    body = {}
    try:
        body = json.loads(event.get("body") or "{}")
    except Exception:
        pass
    device = params.get("device", "") or body.get("device", "")
    serial = params.get("serial", "") or body.get("serial", "")
    if device.startswith(("EMU_", "TEST_")) or serial.startswith(("TEST.", "EA500.E2E")):
        return BUCKET_TEST
    return BUCKET


def respond(status, body):
    return {
        "statusCode": status,
        "headers": {"Content-Type": "application/json"},
        "body": json.dumps(body),
    }


def handler(event, context):
    method = event.get("httpMethod", "")
    path = event.get("path", "")
    params = event.get("queryStringParameters") or {}
    bucket = _pick_bucket(event)

    if path == "/presign" and method == "GET":
        upload_id = params.get("upload_id")

        if not upload_id:
            # ── Start new upload ──────────────────────────────────────
            file_name = params.get("file", "")
            size = int(params.get("size", "0"))
            device = params.get("device", "unknown")

            if not file_name or size <= 0:
                return respond(400, {"error": "file and size required"})

            # Allow caller to specify an explicit S3 key (e.g. aircraft/{serial}/{file})
            key_override = params.get("key", "")
            key = key_override if key_override else f"{device}/{file_name}"

            # Skip upload if S3 already has this file with matching size
            # (e.g. log already uploaded via incremental append)
            try:
                head = s3.head_object(Bucket=bucket, Key=key)
                if head["ContentLength"] >= size:
                    return respond(200, {"skip": True, "key": key,
                                         "reason": "already exists",
                                         "s3_size": head["ContentLength"]})
            except Exception:
                pass  # object doesn't exist — proceed with upload

            if size <= CHUNK:
                # Small file — single pre-signed PUT (no multipart)
                url = s3.generate_presigned_url(
                    "put_object",
                    Params={"Bucket": bucket, "Key": key},
                    ExpiresIn=URL_EXPIRY,
                )
                return respond(200, {"url": url, "upload_id": None, "parts": 1, "key": key})

            # Large file — initiate multipart upload
            mp = s3.create_multipart_upload(Bucket=bucket, Key=key)
            parts = math.ceil(size / CHUNK)
            return respond(200, {
                "upload_id": mp["UploadId"],
                "parts": parts,
                "key": key,
            })

        else:
            # ── Get pre-signed URL for one part ───────────────────────
            key = params.get("key", "")
            part = int(params.get("part", "1"))

            if not key:
                return respond(400, {"error": "key required"})

            url = s3.generate_presigned_url(
                "upload_part",
                Params={
                    "Bucket": bucket,
                    "Key": key,
                    "UploadId": upload_id,
                    "PartNumber": part,
                },
                ExpiresIn=URL_EXPIRY,
            )
            return respond(200, {"url": url})

    elif path == "/firmware" and method == "GET":
        # Return latest firmware version metadata
        try:
            meta = s3.get_object(Bucket=bucket, Key="firmware/latest.json")
            info = json.loads(meta["Body"].read())
            return respond(200, info)
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no firmware available"})
            return respond(500, {"error": err})

    elif path == "/firmware/cookie" and method == "GET":
        # Return DSU cookie as hex-encoded JSON, then delete from S3 (one-shot).
        # Per-device path: firmware/cookies/{device}/dsuCookie.easdf
        device = params.get("device", "")
        if not device:
            return respond(400, {"error": "device required"})
        key = f"firmware/cookies/{device}/dsuCookie.easdf"
        try:
            obj = s3.get_object(Bucket=bucket, Key=key)
            data = obj["Body"].read()
            s3.delete_object(Bucket=bucket, Key=key)
            return respond(200, {"cookie": data.hex(), "size": len(data)})
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no cookie"})
            return respond(500, {"error": err})

    elif path == "/command" and method == "GET":
        # Remote command channel — fetch an airbridge.cmd-syntax command. The device
        # executes it through the SAME command executor as a USB-delivered airbridge.cmd.
        # Key: commands/{device}/airbridge.cmd. Delivery is AT-LEAST-ONCE: the command is
        # NOT deleted on GET (a truncated/lost response on a flaky cellular link would
        # otherwise lose the command forever — seen on hardware at rsrp -111). It is
        # deleted only when the device POSTs /command/ack confirming execution. So a lost
        # GET response just means the next 5-min poll re-fetches. The device's directives
        # (format_sd / reboot / cdc / compress / wifi / s3) are idempotent, so re-running
        # one before an ack lands is harmless.
        device = params.get("device", "")
        if not device:
            return respond(400, {"error": "device required"})
        key = f"commands/{device}/airbridge.cmd"
        try:
            obj = s3.get_object(Bucket=bucket, Key=key)
            data = obj["Body"].read()
            return respond(200, {"cmd": data.decode("utf-8", "replace"), "size": len(data)})
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no command"})
            return respond(500, {"error": err})

    elif path == "/command/ack" and method == "POST":
        # Device confirms it executed a command. Body = result JSON (echoed cmd, per-
        # directive result, fw, reset reason, ts). Stored at commands/{device}/ack.json.
        # Acking ALSO deletes the pending command (delete-on-ack = at-least-once delivery):
        # the command lives until the device proves it ran it, so a lost GET response can't
        # silently drop it.
        device = params.get("device", "")
        if not device:
            return respond(400, {"error": "device required"})
        body = event.get("body", "") or ""
        key = f"commands/{device}/ack.json"
        try:
            s3.put_object(Bucket=bucket, Key=key,
                          Body=body.encode("utf-8"), ContentType="application/json")
            try:
                s3.delete_object(Bucket=bucket, Key=f"commands/{device}/airbridge.cmd")
            except Exception:
                pass  # already gone / nothing pending — ack still succeeds
            return respond(200, {"ok": True})
        except Exception as e:
            return respond(500, {"error": str(e)})

    elif path == "/command/status" and method == "GET":
        # Operator reads the last command ack (acceptance confirmation).
        device = params.get("device", "")
        if not device:
            return respond(400, {"error": "device required"})
        key = f"commands/{device}/ack.json"
        try:
            obj = s3.get_object(Bucket=bucket, Key=key)
            return respond(200, json.loads(obj["Body"].read() or b"{}"))
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no ack"})
            return respond(500, {"error": err})

    elif path == "/command/heartbeat" and method == "POST":
        # Always-on telemetry. The device POSTs a compact JSON snapshot every ~60s
        # in BOTH normal and Safe Mode, over the SD-independent egress path — so a
        # unit whose SD/app is wedged still reports {mode, boots, reset_reason, fw,
        # net, sd, rssi, heap, uptime}. Stored latest-wins at heartbeat/{device}.json
        # so an operator can always SEE a unit's health without scraping logs.
        device = params.get("device", "")
        if not device:
            return respond(400, {"error": "device required"})
        body = event.get("body", "") or ""
        key = f"heartbeat/{device}.json"
        try:
            s3.put_object(Bucket=bucket, Key=key,
                          Body=body.encode("utf-8"), ContentType="application/json")
            return respond(200, {"ok": True})
        except Exception as e:
            return respond(500, {"error": str(e)})

    elif path == "/command/coredump" and method == "POST":
        # A unit that panicked ships its ELF coredump here over the SD-independent
        # cellular path (base64 text body), so a FIELD crash is decodable remotely
        # (esptool + espcoredump) — no physical BOOT-download read ever needed.
        # Stored latest-wins at coredump/{device}.elf; the device erases its on-flash
        # image only after this returns {"ok":true}.
        device = params.get("device", "")
        if not device:
            return respond(400, {"error": "device required"})
        body = event.get("body", "") or ""
        try:
            raw = base64.b64decode(body, validate=False)
            key = f"coredump/{device}.elf"
            s3.put_object(Bucket=bucket, Key=key, Body=raw,
                          ContentType="application/octet-stream")
            return respond(200, {"ok": True, "bytes": len(raw)})
        except Exception as e:
            return respond(500, {"error": str(e)})

    elif path == "/command/heartbeat" and method == "GET":
        # Operator reads a unit's latest heartbeat.
        device = params.get("device", "")
        if not device:
            return respond(400, {"error": "device required"})
        key = f"heartbeat/{device}.json"
        try:
            obj = s3.get_object(Bucket=bucket, Key=key)
            return respond(200, json.loads(obj["Body"].read() or b"{}"))
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no heartbeat"})
            return respond(500, {"error": err})

    elif path == "/firmware/download" and method == "GET":
        # Return pre-signed GET URL for firmware binary
        try:
            url = s3.generate_presigned_url(
                "get_object",
                Params={"Bucket": bucket, "Key": "firmware/latest.bin"},
                ExpiresIn=URL_EXPIRY,
            )
            return respond(200, {"url": url})
        except Exception as e:
            return respond(500, {"error": str(e)})

    elif path == "/complete" and method == "POST":
        body = json.loads(event.get("body", "{}"))
        upload_id = body.get("upload_id", "")
        key = body.get("key", "")
        parts = body.get("parts", [])

        if not upload_id or not key or not parts:
            return respond(400, {"error": "upload_id, key, and parts required"})

        s3.complete_multipart_upload(
            Bucket=bucket,
            Key=key,
            UploadId=upload_id,
            MultipartUpload={
                "Parts": [
                    {"PartNumber": p["part"], "ETag": p["etag"]}
                    for p in parts
                ]
            },
        )
        return respond(200, {"status": "ok"})

    elif path == "/log/append" and method == "POST":
        # Append log text to a per-session S3 object
        # Query: device=XXXX&session=boot_NNNN
        device = params.get("device", "")
        session = params.get("session", "")
        if not device or not session:
            return respond(400, {"error": "device and session required"})

        key = f"{device}/logs/{session}.log"
        body = event.get("body", "")
        if not body:
            return respond(400, {"error": "empty body"})

        # Read existing content (if any) and append
        try:
            existing = s3.get_object(Bucket=bucket, Key=key)["Body"].read()
        except Exception:
            existing = b""

        if isinstance(body, str):
            body = body.encode("utf-8")

        new_content = existing + body
        s3.put_object(Bucket=bucket, Key=key, Body=new_content, ContentType="text/plain")
        return respond(200, {"size": len(new_content)})

    elif path == "/aircraft/manifest" and method == "GET":
        # Return per-DSU manifest. serial must be alphanumeric + dots/dashes.
        serial = params.get("serial", "")
        if not serial or not re.match(r'^[\w.\-]+$', serial):
            return respond(400, {"error": "serial required"})
        mkey = f"aircraft/{serial}/manifest.json"
        try:
            obj = s3.get_object(Bucket=bucket, Key=mkey)
            data = json.loads(obj["Body"].read())
            etag = obj.get("ETag", "").strip('"')
            return {
                "statusCode": 200,
                "headers": {"Content-Type": "application/json", "ETag": etag},
                "body": json.dumps(data),
            }
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                # No manifest yet — return empty state
                return respond(200, {
                    "serial": serial,
                    "high_water_mark": 0,
                    "files": [],
                })
            return respond(500, {"error": err})

    elif path == "/aircraft/cookie" and method == "GET":
        # Return per-aircraft DSU cookie as hex-encoded JSON, then delete (one-shot).
        # Path: aircraft/{serial}/cookie.easdf
        # Used to push date-mode or flight-mode cookies without knowing the device ID.
        serial = params.get("serial", "")
        if not serial or not re.match(r'^[\w.\-]+$', serial):
            return respond(400, {"error": "serial required"})
        key = f"aircraft/{serial}/cookie.easdf"
        try:
            obj = s3.get_object(Bucket=bucket, Key=key)
            data = obj["Body"].read()
            s3.delete_object(Bucket=bucket, Key=key)
            return respond(200, {"cookie": data.hex(), "size": len(data)})
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no cookie"})
            return respond(500, {"error": err})

    elif path == "/aircraft/manifest" and method == "PUT":
        # Admin endpoint: manually set HWM for an aircraft (forward OR backward).
        # PUT /aircraft/manifest?serial=X&hwm=N
        # Setting hwm backward triggers re-collection on the next AirBridge boot.
        serial = params.get("serial", "")
        hwm_str = params.get("hwm", "")
        if not serial or not hwm_str:
            return respond(400, {"error": "serial and hwm required"})
        try:
            new_hwm = int(hwm_str)
        except ValueError:
            return respond(400, {"error": "hwm must be an integer"})
        if not re.match(r'^[\w.\-]+$', serial):
            return respond(400, {"error": "invalid serial"})

        bucket = _pick_bucket(event)
        mkey = f"aircraft/{serial}/manifest.json"
        try:
            obj = s3.get_object(Bucket=bucket, Key=mkey)
            manifest = json.loads(obj["Body"].read())
        except Exception:
            manifest = {"serial": serial, "high_water_mark": 0, "files": []}

        manifest["high_water_mark"] = new_hwm
        manifest["last_updated"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        s3.put_object(Bucket=bucket, Key=mkey, Body=json.dumps(manifest),
                      ContentType="application/json")
        return respond(200, {"serial": serial, "high_water_mark": new_hwm})

    elif path == "/aircraft/manifest" and method == "POST":
        # Update manifest: add a newly uploaded file and advance hwm.
        # Body: {serial, last_flight, first_flight, s3_key}
        try:
            body = json.loads(event.get("body") or "{}")
        except Exception:
            return respond(400, {"error": "invalid JSON body"})
        serial = body.get("serial", "")
        last_flight = int(body.get("last_flight", 0))
        first_flight = int(body.get("first_flight", 0))
        s3_key = body.get("s3_key", "")
        if not serial or last_flight <= 0:
            return respond(400, {"error": "serial and last_flight required"})

        mkey = f"aircraft/{serial}/manifest.json"
        try:
            obj = s3.get_object(Bucket=bucket, Key=mkey)
            manifest = json.loads(obj["Body"].read())
        except Exception:
            manifest = {"serial": serial, "high_water_mark": 0, "files": []}

        # Add file entry (deduplicate by last_flight)
        if s3_key:
            files = manifest.get("files", [])
            if not any(f.get("last_flight") == last_flight for f in files):
                files.append({
                    "key": s3_key,
                    "first_flight": first_flight,
                    "last_flight": last_flight,
                })
            manifest["files"] = files

        # Advance hwm to the highest consecutive flight reachable from current hwm
        hwm = manifest.get("high_water_mark", 0)
        all_files = sorted(manifest["files"], key=lambda x: x.get("first_flight", 0))
        # Bootstrap the floor: a DSU only retains back to some flight (e.g. ~flight 1000
        # for an aircraft a couple years old), so the earliest full download rarely
        # starts at flight 1. Without this, hwm stays 0 forever (no file has
        # first_flight <= hwm+1 == 1) and the skip/dedup optimization never engages.
        # The earliest first_flight ever seen defines the aircraft's floor.
        if hwm == 0 and all_files:
            hwm = all_files[0].get("first_flight", 1) - 1
        changed = True
        while changed:
            changed = False
            for entry in all_files:
                ff = entry.get("first_flight", 1)
                lf = entry.get("last_flight", 0)
                if ff <= hwm + 1 and lf > hwm:
                    hwm = lf
                    changed = True
                    break
        manifest["high_water_mark"] = hwm
        manifest["last_updated"] = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

        s3.put_object(Bucket=bucket, Key=mkey, Body=json.dumps(manifest),
                      ContentType="application/json")
        return respond(200, {"high_water_mark": hwm})

    return respond(404, {"error": "not found"})

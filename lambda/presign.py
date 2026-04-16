"""
Lambda function for AirBridge S3 pre-signed URL generation.

Routes:
  GET  /presign  — Start upload or get a per-part pre-signed URL
  POST /complete — Finalize a multipart upload
"""

import json
import math
import os

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
CHUNK = 5 * 1024 * 1024  # 5 MB — S3 multipart minimum for non-final parts
URL_EXPIRY = 3600  # 1 hour


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

    if path == "/presign" and method == "GET":
        upload_id = params.get("upload_id")

        if not upload_id:
            # ── Start new upload ──────────────────────────────────────
            file_name = params.get("file", "")
            size = int(params.get("size", "0"))
            device = params.get("device", "unknown")

            if not file_name or size <= 0:
                return respond(400, {"error": "file and size required"})

            key = f"{device}/{file_name}"

            if size <= CHUNK:
                # Small file — single pre-signed PUT (no multipart)
                url = s3.generate_presigned_url(
                    "put_object",
                    Params={"Bucket": BUCKET, "Key": key},
                    ExpiresIn=URL_EXPIRY,
                )
                return respond(200, {"url": url, "upload_id": None, "parts": 1, "key": key})

            # Large file — initiate multipart upload
            mp = s3.create_multipart_upload(Bucket=BUCKET, Key=key)
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
                    "Bucket": BUCKET,
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
            meta = s3.get_object(Bucket=BUCKET, Key="firmware/latest.json")
            info = json.loads(meta["Body"].read())
            return respond(200, info)
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no firmware available"})
            return respond(500, {"error": err})

    elif path == "/firmware/cookie" and method == "GET":
        # Return DSU cookie as hex-encoded JSON, then delete from S3 (one-shot)
        try:
            obj = s3.get_object(Bucket=BUCKET, Key="firmware/dsuCookie.easdf")
            data = obj["Body"].read()
            s3.delete_object(Bucket=BUCKET, Key="firmware/dsuCookie.easdf")
            return respond(200, {"cookie": data.hex(), "size": len(data)})
        except Exception as e:
            err = str(e)
            if "NoSuchKey" in err or "404" in err or "AccessDenied" in err:
                return respond(404, {"error": "no cookie"})
            return respond(500, {"error": err})

    elif path == "/firmware/download" and method == "GET":
        # Return pre-signed GET URL for firmware binary
        try:
            url = s3.generate_presigned_url(
                "get_object",
                Params={"Bucket": BUCKET, "Key": "firmware/latest.bin"},
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
            Bucket=BUCKET,
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
            existing = s3.get_object(Bucket=BUCKET, Key=key)["Body"].read()
        except Exception:
            existing = b""

        if isinstance(body, str):
            body = body.encode("utf-8")

        new_content = existing + body
        s3.put_object(Bucket=BUCKET, Key=key, Body=new_content, ContentType="text/plain")
        return respond(200, {"size": len(new_content)})

    return respond(404, {"error": "not found"})

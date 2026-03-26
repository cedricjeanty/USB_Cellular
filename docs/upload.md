# S3 Upload Architecture

```
ESP32 → GET /presign (API Gateway + Lambda) → pre-signed S3 PUT URL
ESP32 → PUT file data → S3 bucket (direct)
```

## AWS Resources (us-west-2)

- **S3 bucket:** `airbridge-uploads`
- **Lambda:** `airbridge-presign` (Python 3.12), source in `lambda/presign.py`
- **API Gateway:** `airbridge-api` (ID: disw6oxjed)
  - Endpoint: `disw6oxjed.execute-api.us-west-2.amazonaws.com`
  - Requires `x-api-key` header (fleet key, rate-limited)
- **IAM role:** `airbridge-presign-lambda`

## Upload Flow

### Small files (< 5 MB): Single PUT
1. ESP32 requests pre-signed PUT URL from Lambda (`GET /presign?file=X&size=Y&device=Z`)
2. Lambda generates `s3.generate_presigned_url("put_object", ...)` and returns it
3. ESP32 does HTTPS PUT directly to S3 with file data
4. Files land in S3 as `<device_mac>/<filename>`

### Large files (≥ 5 MB): S3 Multipart
1. ESP32 requests multipart initiation from Lambda → gets `upload_id`, `key`, `parts` count
2. For each 5 MB part: request part-specific pre-signed URL, PUT chunk to S3
3. ETags from each part stored in NVS for power-loss resume
4. After all parts: POST to `/complete` endpoint with all ETags
5. Lambda calls `s3.complete_multipart_upload()`

## NVS Resume State (namespace `s3up`)

| Key | Purpose |
|-----|---------|
| `name` | Current filename |
| `uid` | S3 multipart upload ID |
| `key` | S3 object key |
| `parts` | Total part count |
| `part` | Next part to upload (1-indexed) |
| `etag1`..`etagN` | ETag for each completed part |

On reboot, if the same file exists in `/harvested/`, upload resumes from the stored part.
Session is auto-cleared on ETag failure or complete failure (stale upload IDs).

## Upload Speed

| Framework | TCP_SND_BUF | Speed |
|-----------|-------------|-------|
| Arduino (esp32-s3) | 5,760 | 90 KB/s |
| ESP-IDF (esp32-idf) | 32,768 | 279 KB/s |

The bottleneck was the pre-compiled lwIP TCP send buffer in the Arduino framework.
ESP-IDF allows custom sdkconfig with larger buffers.

## Key Implementation Details

- Pre-signed URL path can exceed 1600 bytes (AWS STS tokens are long) — buffer is 2500 bytes
- S3 returns ETags with surrounding quotes (`"abc123"`) — must strip before embedding in JSON
- `Connection: close` on all S3 PUT requests (each part gets a new TLS connection)
- ESP-IDF uses `esp_tls` + `esp_crt_bundle_attach` for CA chain validation
- Arduino uses `WiFiClientSecure::setInsecure()` (no cert verification)

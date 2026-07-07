# AirBridge Fleet Dashboard

A single-file ops console for the ESP32-S3 airbridge fleet. It is a **read/write view
over the same API Gateway + Lambda + S3 the devices already use** — there is no separate
database and no connection to the devices. Devices are pull-based (heartbeat every 60 s,
command poll every ~5 min), so the dashboard reads their latest heartbeats and *queues*
commands they pick up on their next poll.

```
Browser (index.html) ──x-api-key──► API Gateway /prod ──► presign.py Lambda ──► S3
        fleet table, drawer,             /fleet /admin/*        heartbeat/ commands/
        command queueing                                        logs/ coredump/ aircraft/
```

## What it shows

- **Fleet table** — every device's latest heartbeat: mode (healthy / **SAFE MODE**),
  cellular state, SD health, firmware (vs. `firmware/latest.json`), RSRP signal meter,
  harvest/upload count, crash/coredump/reset errors, last-seen. Rows are sorted freshest
  first and carry a left severity stripe (green/amber/red, worst-status-first).
- **Device drawer** (click a row) — full heartbeat grid, aircraft serial + manifest
  high-water-mark + files uploaded, coredump presence, a log-session viewer, and a
  **command panel**.
- **Command queueing** — directive buttons (`dump_logs`, `reboot`, `compress`,
  `modem_reset`, `flash`, `format_sd`) or free-text `airbridge.cmd`. Destructive verbs
  prompt to confirm. After queuing it polls the ack so you see *pending → executed*.

## Backend endpoints (added to `lambda/presign.py`)

| Method | Path | Purpose |
|--------|------|---------|
| GET  | `/fleet` | All heartbeats + derived health (staleness, fw-current, coredump, cmd-pending). One call = the home page. `?stale_s=180` sets the offline threshold. |
| GET  | `/admin/device?device=X` | Aggregate: heartbeat + last ack + pending cmd + coredump + aircraft manifest. |
| GET  | `/admin/logs?device=X` | List a device's log sessions. |
| GET  | `/admin/logs?device=X&session=boot_NNNN&tail=N` | Tail one log session. |
| POST | `/admin/command` | Validate directives against the verb whitelist, then write `commands/{device}/airbridge.cmd` and clear the stale ack. Body: `{"device","cmd"}`. |
| DELETE | `/admin/command?device=X` | Cancel a not-yet-fetched command. |

Device→aircraft mapping relies on the **`serial` field added to the heartbeat**
(`main.cpp`, from NVS `mfst/serial`). Until a device ships that firmware, its aircraft
shows "unassigned" — everything else works.

## Security model — private by network, not by secret

A purely static SPA has no server to hold a secret, so the operator API key lives in the
browser (localStorage). That is only safe because **access is locked to your network**:
WAF IP-allowlists gate *both* CloudFront (the page) and the API Gateway (the data) to your
Tailscale exit / home egress IP. Off-network, the key is inert. Do **not** make either the
CloudFront distribution or the API stage openly public with this model — add Cognito first
if you ever need public access.

> Recommended: mint a **separate operator API key** in the API Gateway usage plan (distinct
> from the device firmware key) so you can revoke dashboard access without reflashing the
> fleet.

## Deploy

1. Deploy the updated Lambda (`lambda/presign.py`) — the new routes need no new IAM beyond
   the S3 `ListObjects`/`GetObject`/`PutObject`/`DeleteObject` the function already uses on
   the bucket (confirm `s3:ListBucket` is present for the `/fleet` and `/admin/logs`
   listings).
2. Run `./deploy.sh` (edit the vars at the top first). It creates a private S3 site bucket,
   a CloudFront distribution with a WAF IP-allowlist, and uploads `index.html`. It prints
   the CloudFront URL.
3. Open the URL, click the gear, and enter your API base
   (`https://<api-id>.execute-api.us-west-2.amazonaws.com/prod`) + operator key.
4. Apply the **same** WAF IP set to the API Gateway stage (the script prints the command).

Local use without any hosting: just open `index.html` in a browser and configure the API —
or leave it unconfigured to explore the demo data.

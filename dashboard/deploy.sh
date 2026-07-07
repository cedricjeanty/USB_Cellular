#!/usr/bin/env bash
# Deploy the AirBridge Fleet dashboard as a private static site:
#   S3 (private) + CloudFront (OAC) + WAF IPv4 allowlist.
# Access is gated by network membership (your Tailscale exit / home egress IP),
# NOT by the API key in the browser. Apply the SAME WAF set to the API Gateway stage.
set -euo pipefail

# ── EDIT THESE ───────────────────────────────────────────────────────────────
REGION="us-west-2"
BUCKET="airbridge-fleet-dashboard"          # must be globally unique
ALLOW_CIDRS="1.2.3.4/32"                     # your Tailscale exit / home IP(s), comma-sep
API_STAGE_ARN=""                             # optional: arn:aws:apigateway:...::/restapis/<id>/stages/prod
# ─────────────────────────────────────────────────────────────────────────────
HERE="$(cd "$(dirname "$0")" && pwd)"

echo "▸ Creating private site bucket s3://$BUCKET"
aws s3api create-bucket --bucket "$BUCKET" --region "$REGION" \
  --create-bucket-configuration LocationConstraint="$REGION" 2>/dev/null || true
aws s3api put-public-access-block --bucket "$BUCKET" \
  --public-access-block-configuration \
  BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true

echo "▸ Uploading index.html"
aws s3 cp "$HERE/index.html" "s3://$BUCKET/index.html" \
  --content-type "text/html" --cache-control "no-cache"

# WAF IP set (CLOUDFRONT scope lives in us-east-1)
echo "▸ Creating WAF IPv4 allowlist"
IFS=',' read -ra CIDRS <<< "$ALLOW_CIDRS"
IPSET_ARN=$(aws wafv2 create-ip-set --name airbridge-dash-allow --scope CLOUDFRONT \
  --region us-east-1 --ip-address-version IPV4 \
  --addresses "${CIDRS[@]}" --query 'Summary.ARN' --output text 2>/dev/null \
  || aws wafv2 list-ip-sets --scope CLOUDFRONT --region us-east-1 \
       --query "IPSets[?Name=='airbridge-dash-allow'].ARN" --output text)
echo "  IP set: $IPSET_ARN"

cat <<EOF

▸ Manual steps (one-time, safest done in the console):
  1. Create a WebACL (scope=CLOUDFRONT, region us-east-1) whose default action is BLOCK,
     with one ALLOW rule referencing IP set:
       $IPSET_ARN
  2. Create a CloudFront distribution:
       - Origin: $BUCKET.s3.$REGION.amazonaws.com  (S3, Origin Access Control — the wizard
         writes the bucket policy granting only this distribution GetObject)
       - Default root object: index.html
       - Attach the WebACL from step 1
       - Viewer protocol policy: redirect-to-https
  3. Lock the DATA plane to the same network. WAFv2 can't attach to API Gateway REST APIs
     directly for edge-optimized stages, so use ONE of:
       - Resource policy on the REST API restricting aws:SourceIp to: $ALLOW_CIDRS
       - or a REGIONAL WebACL (scope=REGIONAL, region $REGION) on the stage.
$( [ -n "$API_STAGE_ARN" ] && echo "     Target stage: $API_STAGE_ARN" )

  Example API Gateway resource policy (Console ▸ API ▸ Resource Policy):
    {
      "Version": "2012-10-17",
      "Statement": [{
        "Effect": "Deny", "Principal": "*", "Action": "execute-api:Invoke",
        "Resource": "execute-api:/*",
        "Condition": { "NotIpAddress": { "aws:SourceIp": ["$ALLOW_CIDRS"] } }
      },{
        "Effect": "Allow", "Principal": "*", "Action": "execute-api:Invoke",
        "Resource": "execute-api:/*"
      }]
    }
  (redeploy the stage after saving the policy)

▸ index.html uploaded. Finish steps 1–3, then open the CloudFront URL and configure the
  API base + operator key via the gear icon.
EOF

#!/bin/bash
# Pull the latest instrument_mapping.json snapshot from S3 onto the
# trading host's local disk. Invoked by bpt-secmaster-pull.service
# (oneshot, fired by .timer hourly + at boot).
#
# Atomic write: download to a sibling tempfile in the same directory,
# then rename. bpt-refdata can read the live file concurrently — it
# never sees a partial write.
#
# ETag-conditional GET: skip the download if the local cached ETag
# matches the S3 ETag. Keeps the timer cheap (HEAD request ≈ free).

set -euo pipefail

S3_URI="${S3_URI:-s3://bpt-tape-archive/secmaster/instrument_mapping.json}"
DEST="${DEST:-/opt/bpt/data/instrument_mapping.json}"
ETAG_CACHE="${ETAG_CACHE:-${DEST}.etag}"
AWS_REGION="${AWS_REGION:-ap-northeast-1}"

# Parse bucket + key out of the S3 URI.
without_scheme="${S3_URI#s3://}"
BUCKET="${without_scheme%%/*}"
KEY="${without_scheme#*/}"

mkdir -p "$(dirname "$DEST")"

# Step 1: HEAD object to get current S3 ETag.
remote_etag=$(aws --region "$AWS_REGION" s3api head-object \
    --bucket "$BUCKET" --key "$KEY" \
    --query 'ETag' --output text 2>/dev/null || true)

if [[ -z "$remote_etag" || "$remote_etag" == "None" ]]; then
    echo "fatal: cannot HEAD s3://$BUCKET/$KEY" >&2
    exit 1
fi

# Step 2: compare to local cached ETag. Skip if match.
local_etag=""
[[ -r "$ETAG_CACHE" ]] && local_etag=$(cat "$ETAG_CACHE" 2>/dev/null || true)

if [[ "$remote_etag" == "$local_etag" && -f "$DEST" ]]; then
    echo "no change (etag=$remote_etag); skipping download"
    exit 0
fi

# Step 3: download to sibling tempfile, atomic rename into place.
TMP="${DEST}.tmp.$$"
trap 'rm -f "$TMP"' EXIT

echo "downloading s3://$BUCKET/$KEY (etag $remote_etag) → $DEST"
aws --region "$AWS_REGION" s3api get-object \
    --bucket "$BUCKET" --key "$KEY" \
    "$TMP" --output text >/dev/null

# Sanity: file is non-empty + parses as JSON.
if [[ ! -s "$TMP" ]]; then
    echo "fatal: downloaded file is empty" >&2
    exit 1
fi
if ! python3 -c "import json,sys; json.load(open('$TMP'))" 2>/dev/null; then
    echo "fatal: downloaded file is not valid JSON" >&2
    exit 1
fi

# Atomic rename. fsync first so the rename is durable.
sync "$TMP"
mv -f "$TMP" "$DEST"
echo "$remote_etag" > "$ETAG_CACHE"

bytes=$(stat -c%s "$DEST")
echo "✓ pulled $bytes bytes (etag $remote_etag) → $DEST"

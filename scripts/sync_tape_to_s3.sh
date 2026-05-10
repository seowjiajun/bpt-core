#!/bin/bash
# sync_tape_to_s3.sh — push wslog + parquet files from the local tape host
# to the S3 archive bucket. Triggered hourly by bpt-tape-sync.timer
# (see deploy/generate-units.sh).
#
# Uses `rclone copy` (NOT sync) — never deletes objects on S3 just because
# they vanished locally. The S3 bucket is the long-term archive; local disk
# is hot tier with 7-30 days of working set.
#
# Skips files modified in the last 5 min (--min-age) so the active
# wslog file (currently being written by bpt-tape) isn't uploaded
# half-formed and re-uploaded on the next tick.
#
# Required env (from $BPT_DEPLOY_ROOT/config/active/env):
#   BPT_RAW_ROOT             where wslogs land. Default /opt/bpt/data/raw.
#   BPT_PARQUET_ROOT         where Parquet lands. Default /opt/bpt/data/backtest-cache.
#   BPT_TAPE_S3_BUCKET       target bucket name. Default bpt-tape-archive.
#   BPT_TAPE_S3_REMOTE       rclone remote name. Default bpt-tape (from rclone config).
#
# Idempotent: rclone skips files that are already on S3 with matching
# size + modtime, so re-running is cheap.

set -euo pipefail

RAW_ROOT="${BPT_RAW_ROOT:-/opt/bpt/data/raw}"
PARQUET_ROOT="${BPT_PARQUET_ROOT:-/opt/bpt/data/backtest-cache}"
S3_BUCKET="${BPT_TAPE_S3_BUCKET:-bpt-tape-archive}"
S3_REMOTE="${BPT_TAPE_S3_REMOTE:-bpt-tape}"

log() { echo "[sync $(date -u +%H:%M:%SZ)] $*"; }

# Sanity: rclone must be installed and the remote configured.
if ! command -v rclone >/dev/null; then
    log "FAIL: rclone not installed"
    exit 1
fi
if ! rclone listremotes | grep -q "^${S3_REMOTE}:$"; then
    log "FAIL: rclone remote '${S3_REMOTE}:' not configured. Run rclone config first."
    exit 1
fi

push() {
    local local_path="$1" s3_subdir="$2"
    if [ ! -d "$local_path" ]; then
        log "skip: $local_path does not exist"
        return
    fi
    log "push $local_path → s3://${S3_BUCKET}/${s3_subdir}/"
    rclone copy "$local_path" "${S3_REMOTE}:${S3_BUCKET}/${s3_subdir}" \
        --min-age 5m \
        --transfers 4 \
        --checkers 8 \
        --s3-no-check-bucket \
        --stats=0 \
        --log-level INFO \
        2>&1 | sed "s/^/[sync ${s3_subdir}] /"
}

push "$RAW_ROOT"     "raw"
push "$PARQUET_ROOT" "parquet"

log "sync complete"

# Reclaim disk: prune local copies older than retention that are
# byte-matched on S3. See sync_tape_cleanup.sh for safety properties.
# Exported so the cleanup script picks up the same env without needing
# the EnvironmentFile from the systemd unit (we're already inside it).
script_dir="$(dirname "$(readlink -f "$0")")"
"$script_dir/sync_tape_cleanup.sh"

#!/bin/bash
# sync_tape_cleanup.sh — delete local wslog files older than
# BPT_LOCAL_RETENTION_DAYS, but ONLY after byte-exact verification
# against the S3 archive. Without this script the local volume fills
# silently (root cause of the 2026-05-09 ENOSPC incident — see
# docs/backlog.md "bpt-tape" section).
#
# Called from sync_tape_to_s3.sh at the end of each successful sync,
# so the same hourly timer runs both. Can also be invoked standalone
# for ad-hoc cleanup or with --dry-run to preview.
#
# Required env (defaults match sync_tape_to_s3.sh):
#   BPT_RAW_ROOT             default /opt/bpt/data/raw
#   BPT_TAPE_S3_BUCKET       default bpt-tape-archive
#   BPT_TAPE_S3_REMOTE       default bpt-tape
#   BPT_LOCAL_RETENTION_DAYS default 7
#
# Safety properties:
#   - Never deletes a file unless rclone reports a matching (name, size)
#     under the same S3 prefix. Name + size together are sufficient
#     because the writer never appends — wslogs are immutable once
#     rotated, and rclone copy verifies the size on transfer.
#   - Never touches files newer than the retention window. The retention
#     window must exceed the sync interval (1h) plus the sync's
#     --min-age (5m) so files have had a chance to upload.
#   - Bails on the first integrity mismatch within a date dir, leaving
#     that whole date alone. A partial deletion that ignored a mismatch
#     would mask data loss.
#   - Idempotent: missing local dirs / missing S3 prefix → skip-and-log.

set -euo pipefail

RAW_ROOT="${BPT_RAW_ROOT:-/opt/bpt/data/raw}"
S3_BUCKET="${BPT_TAPE_S3_BUCKET:-bpt-tape-archive}"
S3_REMOTE="${BPT_TAPE_S3_REMOTE:-bpt-tape}"
RETENTION_DAYS="${BPT_LOCAL_RETENTION_DAYS:-7}"

DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --dry-run|-n) DRY_RUN=1 ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "sync_tape_cleanup.sh: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

log() { echo "[cleanup $(date -u +%H:%M:%SZ)] $*"; }

if ! command -v rclone >/dev/null; then
    log "FAIL: rclone not installed"; exit 1
fi
if ! rclone listremotes | grep -q "^${S3_REMOTE}:$"; then
    log "FAIL: rclone remote '${S3_REMOTE}:' not configured"; exit 1
fi
if ! [[ "$RETENTION_DAYS" =~ ^[0-9]+$ ]] || [ "$RETENTION_DAYS" -lt 1 ]; then
    log "FAIL: BPT_LOCAL_RETENTION_DAYS must be a positive integer (got: $RETENTION_DAYS)"
    exit 1
fi

# verify_then_delete <local_dir> <s3_prefix>
# Returns 0 on success (verified-and-deleted, or empty-and-skipped).
# Returns 0 also on safe abort (mismatch logged, dir untouched) — the
# caller doesn't differentiate; the log line is the audit trail.
verify_then_delete() {
    local local_dir="$1" s3_prefix="$2"

    # Snapshot local files: "<name>|<size>" lines, sorted.
    local local_listing
    local_listing="$(find "$local_dir" -mindepth 1 -maxdepth 1 -type f \
        -printf '%f|%s\n' | sort)"
    if [ -z "$local_listing" ]; then
        log "skip $local_dir: empty"
        return 0
    fi

    # S3 listing: same format. `rclone lsf --files-only` excludes dirs.
    local s3_listing
    s3_listing="$(rclone lsf --files-only \
        --format "ps" --separator "|" \
        "${S3_REMOTE}:${S3_BUCKET}/${s3_prefix}" 2>/dev/null | sort || true)"
    if [ -z "$s3_listing" ]; then
        log "WARN $local_dir: S3 prefix ${s3_prefix} is empty — refusing to delete"
        return 0
    fi

    # Validate every local file has a byte-exact match on S3.
    while IFS= read -r line; do
        local name="${line%|*}" size="${line##*|}"
        local s3_size
        s3_size=$(echo "$s3_listing" | awk -F'|' -v n="$name" '$1 == n { print $2; exit }')
        if [ -z "$s3_size" ]; then
            log "MISMATCH $local_dir/$name not on S3 — leaving date dir alone"
            return 0
        fi
        if [ "$s3_size" != "$size" ]; then
            log "MISMATCH $local_dir/$name local=$size S3=$s3_size — leaving date dir alone"
            return 0
        fi
    done <<< "$local_listing"

    # All match. Delete (or pretend to).
    local count=0 bytes=0
    while IFS= read -r line; do
        local name="${line%|*}" size="${line##*|}"
        if [ "$DRY_RUN" -eq 1 ]; then
            log "[dry-run] would rm $local_dir/$name ($size B)"
        else
            rm -f "$local_dir/$name"
        fi
        count=$((count + 1))
        bytes=$((bytes + size))
    done <<< "$local_listing"

    if [ "$DRY_RUN" -eq 1 ]; then
        log "[dry-run] would delete $count files ($((bytes / 1024 / 1024)) MiB) from $local_dir"
    else
        log "deleted $count files ($((bytes / 1024 / 1024)) MiB) from $local_dir"
        rmdir "$local_dir" 2>/dev/null && log "rmdir $local_dir" || true
    fi
}

cleanup_tree() {
    local root="$1" s3_root="$2"
    [ -d "$root" ] || { log "skip $root: not present"; return; }

    # Tree shape: <root>/<venue>/<YYYY-MM-DD>/<file>
    # Prune date-named dirs whose mtime is past the retention window.
    # Directory mtime tracks the last add/remove inside it, so a date
    # whose last hourly wslog was written more than N days ago will
    # match -mtime +N.
    while IFS= read -r date_dir; do
        local base
        base="$(basename "$date_dir")"
        # Defensive: skip anything that's not date-shaped.
        [[ "$base" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || continue
        local rel="${date_dir#$root/}"
        verify_then_delete "$date_dir" "${s3_root}/${rel}"
    done < <(find "$root" -mindepth 2 -maxdepth 2 -type d -mtime "+$RETENTION_DAYS")
}

log "retention=${RETENTION_DAYS}d, raw=${RAW_ROOT}, s3=${S3_REMOTE}:${S3_BUCKET}, dry_run=${DRY_RUN}"
cleanup_tree "$RAW_ROOT" "raw"
log "cleanup complete"

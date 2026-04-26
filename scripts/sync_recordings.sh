#!/bin/bash
# sync_recordings.sh — pull Parquet recordings from a remote recorder
# host into the local backtest cache. Run on the operator's laptop so
# sweeps + the dashboard see fresh data without any cloud dependency
# at sweep time.
#
# Usage:
#   scripts/sync_recordings.sh
#   scripts/sync_recordings.sh --since 2026-04-20      # bound the pull
#   scripts/sync_recordings.sh --dry-run                # just print
#
# Required env (or fill the defaults inline):
#   BPT_RECORDER_HOST   ssh-style "user@host" of the recording VM.
#                       Skip this script entirely if unset (no remote
#                       configured yet — local-only setup).
#   BPT_RECORDER_PORT   ssh port if non-standard. Default 22.
#   BPT_RECORDER_PARQUET_ROOT  remote path to the Parquet tree.
#                              Default /opt/bpt/data/backtest-cache.
#
# Local target: /opt/bpt/data/backtest-cache/  — the same path
# wslog_to_parquet.py writes locally, and what the backtester reads.
# rsync's --update mode means partial syncs from a flaky link don't
# clobber known-good local files with newer-but-incomplete remote
# copies; the recorder host's mtime is authoritative on full files.

set -euo pipefail

DRY_RUN=0
SINCE=""
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --since)   shift; SINCE="$1" ;;
        --since=*) SINCE="${arg#*=}" ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "sync_recordings.sh: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "${BPT_RECORDER_HOST:-}" ]; then
    cat >&2 <<EOF
sync_recordings.sh: BPT_RECORDER_HOST not set.

Set it once in your shell (or in deploy/env/active.env) to enable pulls:
    export BPT_RECORDER_HOST=bpt@your.recorder.example.com

Until then this script is a no-op.
EOF
    exit 0
fi

PORT="${BPT_RECORDER_PORT:-22}"
REMOTE_ROOT="${BPT_RECORDER_PARQUET_ROOT:-/opt/bpt/data/backtest-cache}"
LOCAL_ROOT="/opt/bpt/data/backtest-cache"

mkdir -p "$LOCAL_ROOT"

# rsync flags:
#   -a       archive (preserve perms/timestamps/symlinks)
#   -v       verbose, one line per file
#   -z       compress in transit
#   -P       progress + partial files (resume on link drops)
#   --update keep local file when local mtime > remote (cheap idempotency)
#   --include / --exclude trim to just trades/ + orderbook/ subtrees
#                        if --since given, the date-named .parquet files
#                        outside the window are excluded.
RSYNC_FLAGS=(-avzP --update)

if [ -n "$SINCE" ]; then
    # Convert SINCE (YYYY-MM-DD) to "include only files whose name is
    # >= SINCE.parquet". rsync's include/exclude is glob-only, so we
    # generate a +RW pattern per day from SINCE through today.
    # For typical sync windows (a few days to a few weeks) this stays
    # tractable; longer windows: drop --since and pull everything.
    today=$(date -u +%Y-%m-%d)
    include_args=()
    cursor="$SINCE"
    while [[ "$cursor" < "$today" || "$cursor" == "$today" ]]; do
        include_args+=( --include="*/$cursor.parquet" )
        cursor=$(date -u -d "$cursor + 1 day" +%Y-%m-%d)
    done
    # Allow venue/symbol dirs to recurse, then only the date-matching
    # parquet files inside.
    RSYNC_FLAGS+=( --include='*/' "${include_args[@]}" --exclude='*' )
fi

if [ "$DRY_RUN" = "1" ]; then
    RSYNC_FLAGS+=( --dry-run )
fi

echo "[sync] $BPT_RECORDER_HOST:$REMOTE_ROOT/  →  $LOCAL_ROOT/"
[ -n "$SINCE" ] && echo "[sync] window: $SINCE → $(date -u +%Y-%m-%d) (UTC)"
[ "$DRY_RUN" = "1" ] && echo "[sync] DRY-RUN — no files written"

rsync "${RSYNC_FLAGS[@]}" \
    -e "ssh -p $PORT -o ServerAliveInterval=30 -o ServerAliveCountMax=4" \
    "$BPT_RECORDER_HOST:$REMOTE_ROOT/" \
    "$LOCAL_ROOT/"

echo "[sync] done"

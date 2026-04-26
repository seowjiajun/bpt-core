#!/bin/bash
# rotate_recordings.sh — convert yesterday's .wslog files to Parquet so
# the operator's local sweep tooling can consume them via the standard
# /opt/bpt/data/backtest-cache/ layout that wslog_to_parquet.py writes.
#
# Triggered by the bpt-recording-rotate.timer (see generate-units.sh).
# Designed for a continuous-recording host: bpt-md-recorder.service
# writes /opt/bpt/data/raw/<venue>/<UTC-date>/<venue>-HHMMSS.wslog all
# day, this script wakes up shortly after UTC midnight and converts the
# previous day's files for every venue + symbol mapping the host had
# active.
#
# Idempotent: running it twice on the same day overwrites the same
# Parquet outputs. Skips days that don't have a corresponding raw dir.
#
# Required env (from $BPT_DEPLOY_ROOT/config/active/env or laptop env):
#   BPT_MD_RECORDER_CONFIG   path to the recording TOML — used to
#                            extract the (exchange, symbol) pairs to
#                            convert. Optional: if unset we walk the
#                            raw dir and convert every venue we find.
#   BPT_RAW_ROOT             where wslogs land. Default /opt/bpt/data/raw.
#   BPT_PARQUET_ROOT         where Parquet lands. Default
#                            /opt/bpt/data/backtest-cache.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RAW_ROOT="${BPT_RAW_ROOT:-/opt/bpt/data/raw}"
PARQUET_ROOT="${BPT_PARQUET_ROOT:-/opt/bpt/data/backtest-cache}"

# Yesterday's UTC date — what most of the WS frames will be dated as
# at 00:30 UTC, since the recorder rolls files by UTC date.
DATE_TAG="$(date -u -d 'yesterday' +%Y-%m-%d)"

log() { echo "[rotate $(date -u +%H:%M:%SZ)] $*"; }

if [ ! -d "$RAW_ROOT" ]; then
    log "raw root $RAW_ROOT missing — nothing to rotate"
    exit 0
fi

# Walk every venue dir under raw root. wslog_to_parquet.py auto-derives
# the symbol from the WS frames it sees, so we don't need to enumerate
# instruments here — one call per venue is enough.
shopt -s nullglob
for venue_dir in "$RAW_ROOT"/*/; do
    venue="$(basename "$venue_dir")"
    venue_upper="$(echo "$venue" | tr '[:lower:]' '[:upper:]')"
    day_dir="$venue_dir$DATE_TAG"
    if [ ! -d "$day_dir" ]; then
        log "no $venue dir for $DATE_TAG — skipping"
        continue
    fi

    # Glob the wslogs explicitly so we can short-circuit when the dir
    # is empty (recorder didn't write anything that day for this
    # venue, e.g. WS reconnect storm).
    wslogs=( "$day_dir"/*.wslog )
    if [ ${#wslogs[@]} -eq 0 ]; then
        log "no wslogs in $day_dir — skipping"
        continue
    fi

    log "rotating $venue_upper for $DATE_TAG (${#wslogs[@]} wslog file(s))"
    if python3 "$REPO_ROOT/scripts/wslog_to_parquet.py" \
        --input "$day_dir/*.wslog" \
        --exchange "$venue_upper" \
        --output "$PARQUET_ROOT" 2>&1 | sed "s/^/[rotate $venue_upper] /"; then
        log "ok: $venue_upper $DATE_TAG"
    else
        log "FAILED: $venue_upper $DATE_TAG (wslog_to_parquet.py exit non-zero)"
        # Don't bail — keep going for the remaining venues. A failed
        # day won't auto-retry tomorrow but operator can run this
        # script manually with DATE_TAG override (set the env first).
    fi
done

log "rotate complete"

#!/bin/bash
# Prune stale log files (not modified in > RETENTION_DAYS) across all
# bpt-* service log dirs. Quill handles rotation of actively-written
# logs, but every service rename leaves behind orphan log families
# that never rotate out (e.g. bpt-strategy.log after the rename to
# bpt-strategy, or bpt-strategy.log after the venue-qualification to
# bpt-strat-as). This script catches them.
#
# Safe by design:
#   - Only touches paths matching bpt-<service>/logs/ and related
#   - Only deletes files, never directories
#   - Only deletes .log files (or *.log.*), never .toml / .json / etc
#   - Uses mtime threshold so actively-written files are never touched
#
# Run manually OR via the bpt-log-cleanup.timer (weekly cadence).

set -euo pipefail

BPT_ROOT="${BPT_ROOT:-${HOME}/code/bpt-core}"
RETENTION_DAYS="${RETENTION_DAYS:-30}"

LOG_DIRS=(
    "$BPT_ROOT/bpt-strategy/logs"
    "$BPT_ROOT/bpt-md-gateway/logs"
    "$BPT_ROOT/bpt-order-gateway/logs"
    "$BPT_ROOT/bpt-refdata/logs"
    "$BPT_ROOT/bpt-analytics/logs"
    "$BPT_ROOT/bpt-pricer/logs"
    "$BPT_ROOT/bpt-backtester/logs"
    "$BPT_ROOT/bpt-bridge/logs"
    "$BPT_ROOT/transport/aeron/logs"
)

total_freed=0
total_count=0

for d in "${LOG_DIRS[@]}"; do
    [ -d "$d" ] || continue
    # Find .log files (and rotated .log.N / .N.log variants) older than
    # RETENTION_DAYS. -maxdepth 1 so we don't recurse into backtest/ or
    # other subdirs.
    mapfile -t victims < <(find "$d" -maxdepth 1 -type f \
        \( -name "*.log" -o -name "*.log.*" -o -name "*.stdout" \) \
        -mtime +"$RETENTION_DAYS" 2>/dev/null)
    for f in "${victims[@]}"; do
        sz=$(stat -c%s "$f" 2>/dev/null || echo 0)
        total_freed=$((total_freed + sz))
        total_count=$((total_count + 1))
        rm -f "$f"
        echo "deleted ($(numfmt --to=iec-i --suffix=B $sz)): $f"
    done
done

echo "---"
echo "Summary: $total_count file(s) removed, $(numfmt --to=iec-i --suffix=B $total_freed) freed (retention=${RETENTION_DAYS}d)"

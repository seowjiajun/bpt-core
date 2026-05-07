#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE=bpt-backtester
PID_FILE="$PROJECT_DIR/.$SERVICE.pid"
CONFIG="${1:-$PROJECT_DIR/config/bpt-backtester.qa-okx.toml}"
LOG_FILE="$PROJECT_DIR/logs/$SERVICE.log"
# Prefer installed binary (deployed mode); fall back to CMake build dir (dev mode).
if [ -f "$PROJECT_DIR/bin/$SERVICE" ]; then
    BINARY="$PROJECT_DIR/bin/$SERVICE"
else
    BINARY="$(cd "$PROJECT_DIR/.." && pwd)/build/$SERVICE/src/$SERVICE"
fi
READY_PATTERN="\[bpt-backtester\] Ready"

# ── Guard against double-start ────────────────────────────────────
if [ -f "$PID_FILE" ]; then
    EXISTING_PID=$(cat "$PID_FILE")
    if kill -0 "$EXISTING_PID" 2>/dev/null; then
        echo "$SERVICE is already running (PID $EXISTING_PID)."
        echo "Run ./scripts/stop.sh to stop it first."
        exit 1
    else
        echo "Stale PID file (PID $EXISTING_PID no longer running). Cleaning up."
        rm -f "$PID_FILE"
    fi
fi

# ── Check binary ──────────────────────────────────────────────────
if [ ! -f "$BINARY" ]; then
    echo "ERROR: Binary not found: $BINARY"
    echo "Run: cmake --build build --target bpt-backtester"
    exit 1
fi

mkdir -p "$(dirname "$LOG_FILE")"

# ── Truncate log so startup polling is clean ──────────────────────
> "$LOG_FILE"

echo "Starting $SERVICE..."
echo "  Config : $CONFIG"
echo "  Log    : $LOG_FILE"

cd "$PROJECT_DIR"
# Arrow/Parquet from anaconda3 pull in anaconda's older libstdc++.
# Preload the system one so bpt-backtester's GLIBCXX_3.4.30+ requirements are satisfied.
# BACKTESTER_EXTRA_ARGS (optional env var) forwards CLI flags from the caller
# (e.g. --starting-capital 50000 from backtest.sh / smoke_test.sh).
# shellcheck disable=SC2086
LD_PRELOAD=/lib/x86_64-linux-gnu/libstdc++.so.6 "$BINARY" --config "$CONFIG" ${BACKTESTER_EXTRA_ARGS:-} >> "$LOG_FILE" 2>&1 &
PID=$!
echo "$PID" > "$PID_FILE"

# ── Poll for startup confirmation ─────────────────────────────────
MAX_WAIT=30
elapsed=0
started=false

while [ "$elapsed" -lt "$MAX_WAIT" ]; do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "ERROR: $SERVICE exited early. Check $LOG_FILE for details."
        cat "$LOG_FILE" | tail -20
        rm -f "$PID_FILE"
        exit 1
    fi
    if grep -qE "$READY_PATTERN" "$LOG_FILE" 2>/dev/null; then
        started=true
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

if [ "$started" = true ]; then
    echo "$SERVICE started (PID $PID)."
    echo "  Tail logs : tail -f $LOG_FILE"
    echo "  Stop      : ./scripts/stop.sh"
else
    echo "WARNING: $SERVICE did not confirm startup within ${MAX_WAIT}s (PID $PID)."
    echo "  Check: tail -f $LOG_FILE"
fi

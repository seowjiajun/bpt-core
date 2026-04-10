#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE=surtr_app
DISPLAY_NAME=surtr
PID_FILE="$PROJECT_DIR/.$DISPLAY_NAME.pid"
CONFIG="${1:-$PROJECT_DIR/config/surtr.qa-okx.toml}"
LOG_FILE="$PROJECT_DIR/logs/$DISPLAY_NAME.log"
READY_PATTERN="Starting Surtr Volatility Surface Service"

# Prefer installed binary (deployed mode); fall back to CMake build dir (dev mode).
if [ -f "$PROJECT_DIR/bin/$DISPLAY_NAME" ]; then
    BINARY="$PROJECT_DIR/bin/$DISPLAY_NAME"
else
    BINARY="$(cd "$PROJECT_DIR/.." && pwd)/build/$DISPLAY_NAME/src/$SERVICE"
fi

# ── Guard against double-start ────────────────────────────────────
if [ -f "$PID_FILE" ]; then
    EXISTING_PID=$(cat "$PID_FILE")
    if kill -0 "$EXISTING_PID" 2>/dev/null; then
        echo "$DISPLAY_NAME is already running (PID $EXISTING_PID)."
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
    echo "Run: cmake --build build --target surtr_app"
    exit 1
fi

mkdir -p "$PROJECT_DIR/logs"

# ── Truncate log so startup polling is clean ──────────────────────
> "$LOG_FILE"

echo "Starting $DISPLAY_NAME..."
echo "  Config : $CONFIG"
echo "  Log    : $LOG_FILE"

cd "$PROJECT_DIR"
"$BINARY" "$CONFIG" > /dev/null 2>&1 &
PID=$!
echo "$PID" > "$PID_FILE"

# ── Poll for startup confirmation ─────────────────────────────────
MAX_WAIT=15
elapsed=0
started=false

while [ "$elapsed" -lt "$MAX_WAIT" ]; do
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "ERROR: $DISPLAY_NAME exited early. Check $LOG_FILE for details."
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
    echo "$DISPLAY_NAME started (PID $PID)."
    echo "  Tail logs : tail -f $LOG_FILE"
    echo "  Stop      : ./scripts/stop.sh"
else
    echo "WARNING: $DISPLAY_NAME did not confirm startup within ${MAX_WAIT}s (PID $PID)."
    echo "  Check: tail -f $LOG_FILE"
fi

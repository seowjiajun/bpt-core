#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PID_FILE="$PROJECT_DIR/.bifrost.pid"

# ── Check PID file exists ────────────────────────────────────────
if [ ! -f "$PID_FILE" ]; then
    echo "No PID file found. Bifrost does not appear to be running."
    exit 0
fi

DRIVER_PID=$(cat "$PID_FILE")

# ── Check process is alive ───────────────────────────────────────
if ! kill -0 "$DRIVER_PID" 2>/dev/null; then
    echo "Process $DRIVER_PID is not running. Cleaning up stale PID file."
    rm -f "$PID_FILE"
    exit 0
fi

# ── Graceful shutdown (SIGTERM) ──────────────────────────────────
echo "Stopping Bifrost MediaDriver (PID $DRIVER_PID)..."
kill "$DRIVER_PID"

# ── Wait for exit with timeout ───────────────────────────────────
TIMEOUT=10
ELAPSED=0
while kill -0 "$DRIVER_PID" 2>/dev/null; do
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        echo "Driver did not stop within ${TIMEOUT}s. Sending SIGKILL."
        kill -9 "$DRIVER_PID" 2>/dev/null || true
        break
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

rm -f "$PID_FILE"
echo "Bifrost MediaDriver stopped."

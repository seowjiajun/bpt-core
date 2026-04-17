#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVICE=jormungandr
PID_FILE="$PROJECT_DIR/.$SERVICE.pid"

if [ ! -f "$PID_FILE" ]; then
    echo "No PID file found. $SERVICE does not appear to be running."
    exit 0
fi

PID=$(cat "$PID_FILE")

if ! kill -0 "$PID" 2>/dev/null; then
    echo "Process $PID is not running. Cleaning up stale PID file."
    rm -f "$PID_FILE"
    exit 0
fi

echo "Stopping $SERVICE (PID $PID)..."
kill "$PID"

TIMEOUT=10
ELAPSED=0
while kill -0 "$PID" 2>/dev/null; do
    if [ "$ELAPSED" -ge "$TIMEOUT" ]; then
        echo "$SERVICE did not stop within ${TIMEOUT}s. Sending SIGKILL."
        kill -9 "$PID" 2>/dev/null || true
        break
    fi
    sleep 1
    ELAPSED=$((ELAPSED + 1))
done

rm -f "$PID_FILE"
echo "$SERVICE stopped."

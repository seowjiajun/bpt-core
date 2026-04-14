#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PID_FILE="$PROJECT_DIR/.bifrost.pid"
CONFIG="${1:-$PROJECT_DIR/config/config.yaml}"
LOG_FILE="$PROJECT_DIR/logs/bifrost.log"

# ── Guard against double-start ───────────────────────────────────
if [ -f "$PID_FILE" ]; then
    EXISTING_PID=$(cat "$PID_FILE")
    if kill -0 "$EXISTING_PID" 2>/dev/null; then
        echo "Bifrost is already running (PID $EXISTING_PID)."
        echo "Run ./scripts/dev_stop.sh to stop it first."
        exit 1
    else
        echo "Stale PID file found (PID $EXISTING_PID no longer running). Cleaning up."
        rm -f "$PID_FILE"
    fi
fi

# ── Build fat JAR if missing or stale ────────────────────────────
JAR_FILE=$(find "$PROJECT_DIR/build/libs" -name "*-all.jar" -type f 2>/dev/null | head -1)

if [ -z "$JAR_FILE" ]; then
    echo "Shadow JAR not found. Building..."
    "$PROJECT_DIR/gradlew" -p "$PROJECT_DIR" shadowJar -q
    JAR_FILE=$(find "$PROJECT_DIR/build/libs" -name "*-all.jar" -type f | head -1)
fi

# ── Launch ───────────────────────────────────────────────────────
echo "Starting Bifrost MediaDriver..."
echo "  Config : $CONFIG"
echo "  Log    : $LOG_FILE (logback also writes to logs/bifrost.log)"

mkdir -p "$(dirname "$LOG_FILE")"
# Redirect stdio into the log file and detach from the controlling terminal.
# Without this, the java process inherits the parent shell's stdout/stderr
# (e.g. a terminal, or a pipe to `tail` in a wrapper script) and writes its
# 10-second heartbeat into them, keeping that pipe open forever and
# confusing any pipeline the launcher runs inside. logback writes the real
# structured log into logs/bifrost.log on its own; the redirected file just
# catches stray stdout/stderr from the JVM itself.
java --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
     --add-opens java.base/java.nio=ALL-UNNAMED \
     -jar "$JAR_FILE" --config "$CONFIG" \
     < /dev/null > "$LOG_FILE.stdout" 2>&1 &

DRIVER_PID=$!
echo "$DRIVER_PID" > "$PID_FILE"

# ── Verify it started ────────────────────────────────────────────
# Poll for up to 10 seconds: fail fast if the process exits, succeed early on launch log line.
MAX_WAIT=10
elapsed=0
started=false

while [ "$elapsed" -lt "$MAX_WAIT" ]; do
    if ! kill -0 "$DRIVER_PID" 2>/dev/null; then
        echo "ERROR: MediaDriver process exited early. Check logs/bifrost.log for details."
        rm -f "$PID_FILE"
        exit 1
    fi
    if grep -q "MediaDriver launched successfully" "$LOG_FILE" 2>/dev/null; then
        started=true
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

if [ "$started" = true ]; then
    echo "Bifrost MediaDriver started (PID $DRIVER_PID)."
    echo "  Tail logs : tail -f $LOG_FILE"
    echo "  Stop      : ./scripts/dev_stop.sh"
else
    echo "WARNING: MediaDriver did not confirm startup within ${MAX_WAIT}s (PID $DRIVER_PID)."
    echo "  It may still be initializing. Check: tail -f $LOG_FILE"
    echo "  Stop : ./scripts/dev_stop.sh"
fi

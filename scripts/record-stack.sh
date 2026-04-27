#!/bin/bash
# Launch the recording rig — dedicated MediaDriver + standalone md-gateway
# with [recording] enabled. md-gateway tees raw WS frames to disk inside
# its own venue adapters; no separate recorder process.
#
# Outputs:
#   /opt/bpt/data/raw/{venue}/YYYY-MM-DD/{venue}-HHMMSS.wslog
#       — raw venue WS frames (JSON for OKX/HL/Binance, FIX for Deribit)
#         interleaved with SESSION_START/STOP/CHECKPOINT markers.
#
# Zero coupling to the trading stack — uses /dev/shm/aeron-bpt-record.
#
# Usage:
#   ./scripts/record-stack.sh up [config]   Start the two processes
#   ./scripts/record-stack.sh down          Stop them
#   ./scripts/record-stack.sh status        Show PIDs
#
# Optional config arg (path or shorthand):
#   okx (default) → bpt-md-gateway.standalone-okx.toml
#   hl            → bpt-md-gateway.recording-hl.toml (HL mainnet)
#   <path>        → use that TOML directly

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"

TRANSPORT_DIR="$REPO/transport/aeron"
TRANSPORT_CONFIG="$TRANSPORT_DIR/config/config-record.yaml"
TRANSPORT_PID="$REPO/.record-transport.pid"
TRANSPORT_LOG="$REPO/logs/record-transport.log"

resolve_config() {
    case "${1:-okx}" in
        okx) echo "$REPO/bpt-md-gateway/config/bpt-md-gateway.standalone-okx.toml" ;;
        hl)  echo "$REPO/bpt-md-gateway/config/bpt-md-gateway.recording-hl.toml" ;;
        /*)  echo "$1" ;;
        *)   echo "$REPO/$1" ;;
    esac
}

MDGW_BIN="$REPO/bazel-bin/bpt-md-gateway/bpt-md-gateway"
MDGW_CONFIG=""  # set by cmd_up from the second positional arg
MDGW_PID="$REPO/.record-mdgw.pid"
MDGW_LOG="$REPO/logs/record-mdgw.log"

mkdir -p "$REPO/logs" /opt/bpt/data/raw 2>/dev/null || true

cmd_up() {
    MDGW_CONFIG="$(resolve_config "${1:-okx}")"
    if [ ! -f "$MDGW_CONFIG" ]; then
        echo "config not found: $MDGW_CONFIG"
        exit 1
    fi

    # 1. MediaDriver
    if [ -f "$TRANSPORT_PID" ] && kill -0 "$(cat "$TRANSPORT_PID")" 2>/dev/null; then
        echo "transport already running (PID $(cat "$TRANSPORT_PID"))"
    else
        local jar
        jar=$(find "$TRANSPORT_DIR/build/libs" -name "*-all.jar" -type f 2>/dev/null | head -1)
        if [ -z "$jar" ]; then
            echo "Building bifrost-fabric shadowJar..."
            "$TRANSPORT_DIR/gradlew" -p "$TRANSPORT_DIR" shadowJar -q
            jar=$(find "$TRANSPORT_DIR/build/libs" -name "*-all.jar" -type f | head -1)
        fi
        echo "Starting MediaDriver → $TRANSPORT_CONFIG"
        nohup java \
            --add-opens java.base/sun.nio.ch=ALL-UNNAMED \
            --add-opens java.base/java.nio=ALL-UNNAMED \
            -jar "$jar" --config "$TRANSPORT_CONFIG" \
            </dev/null >"$TRANSPORT_LOG" 2>&1 &
        echo $! >"$TRANSPORT_PID"
        elapsed=0
        while [ "$elapsed" -lt 15 ]; do
            if ! kill -0 "$(cat "$TRANSPORT_PID")" 2>/dev/null; then
                echo "MediaDriver failed — see $TRANSPORT_LOG"
                exit 1
            fi
            if grep -q "MediaDriver launched successfully" "$TRANSPORT_LOG" 2>/dev/null; then
                break
            fi
            sleep 1
            elapsed=$((elapsed + 1))
        done
    fi

    # 2. Build md-gateway if missing
    bazel build //bpt-md-gateway:bpt-md-gateway

    # 3. md-gateway with recording
    if [ -f "$MDGW_PID" ] && kill -0 "$(cat "$MDGW_PID")" 2>/dev/null; then
        echo "md-gateway already running (PID $(cat "$MDGW_PID"))"
    else
        echo "Starting md-gateway (recording on) → $MDGW_CONFIG"
        nohup "$MDGW_BIN" --config "$MDGW_CONFIG" >"$MDGW_LOG" 2>&1 &
        echo $! >"$MDGW_PID"
        sleep 3
        if ! kill -0 "$(cat "$MDGW_PID")" 2>/dev/null; then
            echo "md-gateway failed — see $MDGW_LOG"
            exit 1
        fi
    fi

    echo
    echo "Recording rig up."
    echo "  Transport : PID $(cat "$TRANSPORT_PID"), log $TRANSPORT_LOG"
    echo "  MD gateway: PID $(cat "$MDGW_PID"), log $MDGW_LOG"
    echo
    echo "Output   : /opt/bpt/data/raw/<venue>/<date>/*.wslog"
    echo "Convert  : python3 scripts/wslog_to_parquet.py --input '/opt/bpt/data/raw/**/*.wslog' --exchange <VENUE>"
    echo "Stop     : $0 down"
}

cmd_down() {
    # Stop md-gateway BEFORE transport so the SESSION_STOP marker can be written
    # before the Aeron driver tears down (md-gateway flushes spool on shutdown).
    for pidfile in "$MDGW_PID" "$TRANSPORT_PID"; do
        if [ -f "$pidfile" ]; then
            pid=$(cat "$pidfile")
            if kill -0 "$pid" 2>/dev/null; then
                echo "Stopping PID $pid ($(basename "$pidfile"))"
                kill "$pid"
                # md-gateway shutdown drains WS reconnect-retry + IO thread join
                # before writing SESSION_STOP — give it 30s before SIGKILL so
                # the spool gets a clean closing marker.
                for i in $(seq 1 30); do
                    kill -0 "$pid" 2>/dev/null || break
                    sleep 1
                done
                kill -9 "$pid" 2>/dev/null || true
            fi
            rm -f "$pidfile"
        fi
    done
    echo "Recording rig down."
}

cmd_status() {
    for name_pid in "transport:$TRANSPORT_PID" "mdgw:$MDGW_PID"; do
        name="${name_pid%%:*}"
        pidfile="${name_pid#*:}"
        if [ -f "$pidfile" ] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
            echo "$name: running (PID $(cat "$pidfile"))"
        else
            echo "$name: not running"
        fi
    done
}

case "${1:-}" in
    up)     cmd_up "${2:-okx}" ;;
    down)   cmd_down ;;
    status) cmd_status ;;
    *) echo "Usage: $0 {up [okx|hl|<path>]|down|status}"; exit 1 ;;
esac

#!/bin/bash
# smoke_test.sh — End-to-end dashboard smoke test.
#
# Launches the full backtest stack (bifrost + muninn + huginn + heimdall +
# fenrir + jormungandr) via the existing backtest.sh, then starts the bridge
# on top and prints the command to run the frontend.
#
# Usage:
#   ./smoke_test.sh start [fenrir-config]  Start stack + bridge
#   ./smoke_test.sh stop                   Stop bridge + stack
#   ./smoke_test.sh status                 Show what's running
#   ./smoke_test.sh verify                 Compare bridge equity vs summary.json
#
# The optional fenrir-config is forwarded to backtest.sh — use it to pick a
# strategy config other than the default vwap_reversion.backtest.toml.
#
# What a passing smoke test looks like:
#   1. Stack launches, Jormungandr starts replaying Jan 6-12 OKX data
#   2. Bridge connects to Aeron, listens on :8080
#   3. Frontend (run separately) connects to ws://localhost:8080
#   4. Blotter fills up with 22 fills as the backtest runs
#   5. Final equity on the dashboard matches summary.json within $0.01

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BRIDGE_BIN="$STACK_DIR/build/dashboard/bridge/bridge"
BRIDGE_CFG="$STACK_DIR/dashboard/bridge/config/bridge.backtest.toml"
BRIDGE_LOG_DIR="$STACK_DIR/dashboard/bridge/logs"
BRIDGE_PID="$STACK_DIR/dashboard/bridge/.bridge.pid"
BACKTEST_SH="$STACK_DIR/scripts/backtest.sh"
RESULTS_DIR="$STACK_DIR/jormungandr/results"
FRONTEND_DIR="$STACK_DIR/dashboard/frontend"

is_running() {
    local pid_file="$1"
    [ -f "$pid_file" ] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

# Extract a display name for the running strategy from the fenrir config
# filename.  momentum.backtest.toml  →  MomentumStrategy
# vwap_reversion.backtest.toml       →  VwapReversionStrategy
derive_strategy_name() {
    local cfg="$1"
    [ -z "$cfg" ] && { echo "unknown"; return; }
    local base
    base="$(basename "$cfg")"          # vwap_reversion.backtest.toml
    base="${base%%.backtest.toml}"     # vwap_reversion
    base="${base%%.*}"                 # vwap_reversion (safety)
    # Convert snake_case → PascalCase and append "Strategy"
    awk 'BEGIN{FS="_"; OFS=""}
         { for (i=1;i<=NF;i++) $i=toupper(substr($i,1,1)) substr($i,2); print $0 "Strategy" }' <<<"$base"
}

bridge_start() {
    if is_running "$BRIDGE_PID"; then
        echo "  [UP]   bridge (PID $(cat "$BRIDGE_PID")) — already running"
        return 0
    fi

    if [ ! -x "$BRIDGE_BIN" ]; then
        echo "ERROR: bridge binary not found at $BRIDGE_BIN"
        echo "       build it first: cmake --build build -j\$(nproc) --target bridge_app"
        exit 1
    fi

    if [ ! -f "$BRIDGE_CFG" ]; then
        echo "ERROR: bridge config not found at $BRIDGE_CFG"
        exit 1
    fi

    mkdir -p "$BRIDGE_LOG_DIR"

    local strategy_name
    strategy_name="$(derive_strategy_name "${FENRIR_CONFIG_OVERRIDE:-}")"

    echo "  Starting bridge (strategy: $strategy_name)..."
    nohup "$BRIDGE_BIN" --config "$BRIDGE_CFG" --strategy-name "$strategy_name" \
        > "$BRIDGE_LOG_DIR/bridge.stdout" 2>&1 &
    echo $! > "$BRIDGE_PID"

    sleep 1
    if is_running "$BRIDGE_PID"; then
        echo "  [UP]   bridge (PID $(cat "$BRIDGE_PID"))"
    else
        echo "  [FAIL] bridge did not start — check $BRIDGE_LOG_DIR/bridge.stdout"
        rm -f "$BRIDGE_PID"
        exit 1
    fi
}

bridge_stop() {
    if is_running "$BRIDGE_PID"; then
        local pid
        pid=$(cat "$BRIDGE_PID")
        echo "  Stopping bridge (PID $pid)..."
        kill "$pid" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            is_running "$BRIDGE_PID" || break
            sleep 0.5
        done
        is_running "$BRIDGE_PID" && kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$BRIDGE_PID"
    echo "  [DOWN] bridge"
}

do_status() {
    "$BACKTEST_SH" status
    if is_running "$BRIDGE_PID"; then
        echo "  [UP]   bridge (PID $(cat "$BRIDGE_PID"))"
    else
        echo "  [DOWN] bridge"
    fi
}

do_start() {
    echo "=== Dashboard smoke test — starting ==="
    echo

    # Forward the optional fenrir-config override to backtest.sh
    if [ -n "${FENRIR_CONFIG_OVERRIDE:-}" ]; then
        "$BACKTEST_SH" start "$FENRIR_CONFIG_OVERRIDE"
    else
        "$BACKTEST_SH" start
    fi
    echo

    bridge_start
    echo

    cat <<EOF
=== Smoke test stack is up ===

Now, in a separate terminal, start the frontend pointed at the bridge:

    cd $FRONTEND_DIR
    VITE_WS_URL=ws://localhost:8080 npm run dev

Then open the URL Vite prints (http://localhost:5173/).  The dashboard
should animate with real fills as Jormungandr replays the data.

Logs:
  bridge  : $BRIDGE_LOG_DIR/bridge.stdout
  fenrir  : $STACK_DIR/fenrir/logs/fenrir.log
  jormun. : $STACK_DIR/jormungandr/logs/backtest/jormungandr.log

After the backtest completes, run:
    $0 verify

to check that the bridge-computed equity matches the reference summary.json.
EOF
}

do_stop() {
    echo "=== Dashboard smoke test — stopping ==="
    bridge_stop
    "$BACKTEST_SH" stop
    echo "=== Stack is down ==="
}

do_verify() {
    local latest
    latest=$(ls -1dt "$RESULTS_DIR"/*/ 2>/dev/null | head -1)
    if [ -z "$latest" ]; then
        echo "ERROR: no results directory found under $RESULTS_DIR"
        exit 1
    fi

    local summary="$latest/summary.json"
    if [ ! -f "$summary" ]; then
        echo "ERROR: summary.json missing in $latest"
        exit 1
    fi

    echo "Reference run : $latest"
    python3 - <<PY
import json
with open("$summary") as f:
    s = json.load(f)
print(f"  starting capital : \${s['starting_capital']:>12,.2f}")
print(f"  final equity     : \${s['final_equity']:>12,.2f}")
print(f"  total PnL        : \${s['total_pnl']:>12,.2f}")
print(f"  return           : {s['return_pct']:>12.4f}%")
print(f"  total fills      : {s['total_fills']:>12}")
PY

    echo
    echo "To verify against the dashboard:"
    echo "  1. Open $latest/summary.json in one window"
    echo "  2. Open the dashboard in another"
    echo "  3. Confirm total_fills, final_equity, and total_pnl all match"
}

case "${1:-}" in
    start)
        FENRIR_CONFIG_OVERRIDE="${2:-}"
        do_start
        ;;
    stop)   do_stop ;;
    status) do_status ;;
    verify) do_verify ;;
    *)
        echo "Usage: $0 {start|stop|status|verify} [fenrir-config]"
        exit 1
        ;;
esac

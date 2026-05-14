#!/bin/bash
# paper_run.sh — Launch the paper trading stack + dashboard bridge.
#
# Mirror of smoke_test.sh but for paper trading against OKX testnet.
# The dashboard URL is the same — what changes is the mode pill in the
# top bar (yellow "PAPER" instead of blue "BACKTEST").
#
# Usage:
#   ./paper_run.sh start [strategy-config] [--instrument-id N]
#   ./paper_run.sh stop
#   ./paper_run.sh status
#
# Default strategy-config: bpt-strategy/config/vwap_reversion.qa-okx.toml
#
# --instrument-id restricts the dashboard to a single instrument when the
# bpt-strategy strategy trades multiple.  Without it, fills from every instrument
# get mixed into one blotter/position/equity view, which is visually wrong.
# OKX instrument IDs:  BTC-USDT=200102  ETH-USDT=200202  SOL-USDT=200302
#                      BNB-USDT=200402  XRP-USDT=200902

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BRIDGE_BIN="$STACK_DIR/build/bpt-bridge/bpt-bridge"
BRIDGE_CFG="$STACK_DIR/bpt-bridge/config/bridge.live.toml"
BRIDGE_LOG_DIR="$STACK_DIR/bpt-bridge/logs"
BRIDGE_PID="$STACK_DIR/bpt-bridge/.bridge.pid"
PAPER_SH="$STACK_DIR/scripts/paper.sh"
FRONTEND_DIR="$STACK_DIR/dashboard/frontend"
FRONTEND_BIN="$FRONTEND_DIR/node_modules/.bin/vite"
FRONTEND_LOG_DIR="$FRONTEND_DIR/logs"
FRONTEND_LOG="$FRONTEND_LOG_DIR/vite.stdout"
FRONTEND_PID="$FRONTEND_DIR/.vite.pid"

# TODO(prod): this orchestrator runs vite in dev mode (HMR, on-demand
# TS/JSX compilation, dev error overlay) for fast iteration. For a real
# prod deployment, replace with `npm run build` + a static file server
# (nginx/caddy) serving dist/ on port 80/443. Dev mode is not suitable
# for anything past a single-machine local stack.

is_running() {
    local pid_file="$1"
    [ -f "$pid_file" ] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

# Extract a display name for the running strategy from the bpt-strategy config
# filename.  vwap_reversion.qa-okx.toml → VwapReversionStrategy
derive_strategy_name() {
    local cfg="$1"
    [ -z "$cfg" ] && { echo "unknown"; return; }
    local base
    base="$(basename "$cfg")"
    base="${base%%.*}"
    base="${base%%.*}"
    base="${base%%.*}"
    awk 'BEGIN{FS="_"; OFS=""}
         { for (i=1;i<=NF;i++) $i=toupper(substr($i,1,1)) substr($i,2); print $0 "Strategy" }' <<<"$base"
}

# Derive display metadata (symbol/exchange/instrument-type) from the bpt-strategy
# config filename so the bridge top-bar shows the right labels without
# having to hand-pass --symbol / --exchange / --instrument-type every time.
# These are display-only; they don't affect trading.
derive_display_info() {
    local cfg="$1"
    DISPLAY_SYMBOL="BTC-USDT"
    DISPLAY_EXCHANGE="OKX"
    DISPLAY_INSTRUMENT_TYPE="SPOT"

    case "$(basename "$cfg")" in
        *hyperliquid*)
            DISPLAY_SYMBOL="BTC"
            DISPLAY_EXCHANGE="HYPERLIQUID"
            DISPLAY_INSTRUMENT_TYPE="PERP" ;;
        *deribit*)
            DISPLAY_SYMBOL="BTC-PERPETUAL"
            DISPLAY_EXCHANGE="DERIBIT"
            DISPLAY_INSTRUMENT_TYPE="PERP" ;;
        *okx*)
            DISPLAY_SYMBOL="BTC-USDT"
            DISPLAY_EXCHANGE="OKX"
            DISPLAY_INSTRUMENT_TYPE="SPOT" ;;
    esac
}

bridge_start() {
    if is_running "$BRIDGE_PID"; then
        echo "  [UP]   bridge (PID $(cat "$BRIDGE_PID")) — already running"
        return 0
    fi

    if [ ! -x "$BRIDGE_BIN" ]; then
        echo "ERROR: bridge binary not found at $BRIDGE_BIN"
        exit 1
    fi

    mkdir -p "$BRIDGE_LOG_DIR"

    local strategy_name
    strategy_name="$(derive_strategy_name "${STRATEGY_CONFIG_OVERRIDE:-}")"
    derive_display_info "${STRATEGY_CONFIG_OVERRIDE:-}"

    local extra_args=()
    if [ -n "${INSTRUMENT_ID:-}" ]; then
        extra_args+=(--instrument-id "$INSTRUMENT_ID")
    fi

    echo "  Starting bridge (mode: paper, strategy: $strategy_name, symbol: $DISPLAY_SYMBOL@$DISPLAY_EXCHANGE/$DISPLAY_INSTRUMENT_TYPE${INSTRUMENT_ID:+, instrument_id: $INSTRUMENT_ID})..."

    # setsid puts the child in its own session so it survives any TTY the
    # parent shell might own. Redirecting stdio to /dev/null + logfile
    # fully detaches it.
    #
    # setsid fork-execs into the bridge, which means the captured `$!` is
    # the PID of setsid (which exits immediately after exec), NOT the
    # bridge. We have to look up the real PID with pgrep after the spawn.
    setsid "$BRIDGE_BIN" --config "$BRIDGE_CFG" \
                         --mode paper \
                         --strategy-name "$strategy_name" \
                         --symbol "$DISPLAY_SYMBOL" \
                         --exchange "$DISPLAY_EXCHANGE" \
                         --instrument-type "$DISPLAY_INSTRUMENT_TYPE" \
                         "${extra_args[@]}" \
        < /dev/null > "$BRIDGE_LOG_DIR/bridge.stdout" 2>&1 &
    disown 2>/dev/null || true

    # Give setsid a moment to exec into bridge, then look it up by exe path.
    local actual_pid=""
    for _ in 1 2 3 4 5; do
        sleep 0.3
        actual_pid="$(pgrep -fx "$BRIDGE_BIN .*" 2>/dev/null | head -1 || true)"
        [ -n "$actual_pid" ] && break
    done

    if [ -n "$actual_pid" ] && kill -0 "$actual_pid" 2>/dev/null; then
        echo "$actual_pid" > "$BRIDGE_PID"
        echo "  [UP]   bridge (PID $actual_pid)"
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

frontend_start() {
    if is_running "$FRONTEND_PID"; then
        echo "  [UP]   frontend (PID $(cat "$FRONTEND_PID")) — already running"
        return 0
    fi

    if [ ! -x "$FRONTEND_BIN" ]; then
        echo "ERROR: vite not found at $FRONTEND_BIN"
        echo "       run 'cd $FRONTEND_DIR && npm install' first"
        exit 1
    fi

    mkdir -p "$FRONTEND_LOG_DIR"

    echo "  Starting frontend (vite dev server on :5173, ws→localhost:8080)..."

    # Invoke vite directly (it's a node shebang script) rather than
    # going through `npm run dev` — avoids the npm wrapper process and
    # makes the real PID easier to track via pgrep.
    (
        cd "$FRONTEND_DIR"
        VITE_WS_URL=ws://localhost:8080 \
            setsid "$FRONTEND_BIN" \
            < /dev/null > "$FRONTEND_LOG" 2>&1 &
        disown 2>/dev/null || true
    )

    # setsid fork-execs into node, so $! is the stale setsid PID.
    # Look up the real vite PID by its cmdline.
    local actual_pid=""
    for _ in 1 2 3 4 5 6 7 8; do
        sleep 0.5
        actual_pid="$(pgrep -f "node.*$FRONTEND_BIN" 2>/dev/null | head -1 || true)"
        [ -n "$actual_pid" ] && break
    done

    if [ -n "$actual_pid" ] && kill -0 "$actual_pid" 2>/dev/null; then
        echo "$actual_pid" > "$FRONTEND_PID"
        echo "  [UP]   frontend (PID $actual_pid)"
    else
        echo "  [FAIL] frontend did not start — check $FRONTEND_LOG"
        rm -f "$FRONTEND_PID"
        exit 1
    fi
}

frontend_stop() {
    if is_running "$FRONTEND_PID"; then
        local pid
        pid=$(cat "$FRONTEND_PID")
        echo "  Stopping frontend (PID $pid)..."
        kill "$pid" 2>/dev/null || true
        for _ in 1 2 3 4 5; do
            is_running "$FRONTEND_PID" || break
            sleep 0.5
        done
        is_running "$FRONTEND_PID" && kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$FRONTEND_PID"
    echo "  [DOWN] frontend"
}

do_status() {
    "$PAPER_SH" status
    if is_running "$BRIDGE_PID"; then
        echo "  [UP]   bridge (PID $(cat "$BRIDGE_PID"))"
    else
        echo "  [DOWN] bridge"
    fi
    if is_running "$FRONTEND_PID"; then
        echo "  [UP]   frontend (PID $(cat "$FRONTEND_PID"))"
    else
        echo "  [DOWN] frontend"
    fi
}

do_start() {
    echo "=== Dashboard paper trading run — starting ==="
    echo

    if [ -n "${STRATEGY_CONFIG_OVERRIDE:-}" ]; then
        "$PAPER_SH" start "$STRATEGY_CONFIG_OVERRIDE"
    else
        "$PAPER_SH" start
    fi
    echo

    bridge_start
    echo

    frontend_start
    echo

    cat <<EOF
=== Paper trading stack is up ===

  Dashboard : http://localhost:5173           (yellow PAPER pill)
  Bridge WS : ws://localhost:8080

Logs:
  bridge   : $BRIDGE_LOG_DIR/bridge.stdout
  frontend : $FRONTEND_LOG
  bpt-strategy   : $STACK_DIR/bpt-strategy/logs/bpt-strategy.log
EOF
}

do_stop() {
    echo "=== Dashboard paper trading run — stopping ==="
    frontend_stop
    bridge_stop
    "$PAPER_SH" stop
    echo "=== Stack is down ==="
}

# ── Arg parsing ────────────────────────────────────────────────────────────
SUBCMD="${1:-}"
shift || true

STRATEGY_CONFIG_OVERRIDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --instrument-id)      INSTRUMENT_ID="$2"; shift 2 ;;
        --instrument-id=*)    INSTRUMENT_ID="${1#--instrument-id=}"; shift ;;
        -*)                   echo "Unknown flag: $1"; exit 1 ;;
        *)
            if [ -z "$STRATEGY_CONFIG_OVERRIDE" ]; then
                STRATEGY_CONFIG_OVERRIDE="$1"
            fi
            shift
            ;;
    esac
done
export INSTRUMENT_ID

case "$SUBCMD" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 {start|stop|status} [strategy-config] [--instrument-id N]"
        exit 1
        ;;
esac

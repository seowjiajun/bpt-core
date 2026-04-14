#!/bin/bash
# paper_run.sh — Launch the paper trading stack + dashboard bridge.
#
# Mirror of smoke_test.sh but for paper trading against OKX testnet.
# The dashboard URL is the same — what changes is the mode pill in the
# top bar (yellow "PAPER" instead of blue "BACKTEST").
#
# Usage:
#   ./paper_run.sh start [fenrir-config] [--instrument-id N]
#   ./paper_run.sh stop
#   ./paper_run.sh status
#
# Default fenrir-config: fenrir/config/vwap_reversion.qa-okx.toml
#
# --instrument-id restricts the dashboard to a single instrument when the
# fenrir strategy trades multiple.  Without it, fills from every instrument
# get mixed into one blotter/position/equity view, which is visually wrong.
# OKX instrument IDs:  BTC-USDT=200102  ETH-USDT=200202  SOL-USDT=200302
#                      BNB-USDT=200402  XRP-USDT=200902

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
BRIDGE_BIN="$STACK_DIR/build/dashboard/bridge/bridge"
BRIDGE_CFG="$STACK_DIR/dashboard/bridge/config/bridge.live.toml"
BRIDGE_LOG_DIR="$STACK_DIR/dashboard/bridge/logs"
BRIDGE_PID="$STACK_DIR/dashboard/bridge/.bridge.pid"
PAPER_SH="$STACK_DIR/scripts/paper.sh"
FRONTEND_DIR="$STACK_DIR/dashboard/frontend"

is_running() {
    local pid_file="$1"
    [ -f "$pid_file" ] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

# Extract a display name for the running strategy from the fenrir config
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

# Derive display metadata (symbol/exchange/instrument-type) from the fenrir
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
    strategy_name="$(derive_strategy_name "${FENRIR_CONFIG_OVERRIDE:-}")"
    derive_display_info "${FENRIR_CONFIG_OVERRIDE:-}"

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

do_status() {
    "$PAPER_SH" status
    if is_running "$BRIDGE_PID"; then
        echo "  [UP]   bridge (PID $(cat "$BRIDGE_PID"))"
    else
        echo "  [DOWN] bridge"
    fi
}

do_start() {
    echo "=== Dashboard paper trading run — starting ==="
    echo

    if [ -n "${FENRIR_CONFIG_OVERRIDE:-}" ]; then
        "$PAPER_SH" start "$FENRIR_CONFIG_OVERRIDE"
    else
        "$PAPER_SH" start
    fi
    echo

    bridge_start
    echo

    cat <<EOF
=== Paper trading stack is up ===

In a separate terminal, start the frontend pointed at the bridge:

    cd $FRONTEND_DIR
    VITE_WS_URL=ws://localhost:8080 npm run dev

The top bar will show a yellow PAPER pill.  Real market data from OKX
testnet flows into Huginn; Fenrir's orders hit the OKX demo-trading
endpoint via Heimdall; fills come back through the same Aeron stream
the backtest used, so the dashboard looks and behaves identically.

Logs:
  bridge  : $BRIDGE_LOG_DIR/bridge.stdout
  fenrir  : $STACK_DIR/fenrir/logs/fenrir.log
EOF
}

do_stop() {
    echo "=== Dashboard paper trading run — stopping ==="
    bridge_stop
    "$PAPER_SH" stop
    echo "=== Stack is down ==="
}

# ── Arg parsing ────────────────────────────────────────────────────────────
SUBCMD="${1:-}"
shift || true

FENRIR_CONFIG_OVERRIDE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --instrument-id)      INSTRUMENT_ID="$2"; shift 2 ;;
        --instrument-id=*)    INSTRUMENT_ID="${1#--instrument-id=}"; shift ;;
        -*)                   echo "Unknown flag: $1"; exit 1 ;;
        *)
            if [ -z "$FENRIR_CONFIG_OVERRIDE" ]; then
                FENRIR_CONFIG_OVERRIDE="$1"
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
        echo "Usage: $0 {start|stop|status} [fenrir-config] [--instrument-id N]"
        exit 1
        ;;
esac

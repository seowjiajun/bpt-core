#!/bin/bash
# paper.sh — Run the Strategy paper trading stack against OKX testnet.
#
# Paper trading uses real market data from the exchange's testnet endpoints
# with simulated fills against a testnet account.  No Backtester — data
# comes straight from OKX's demo-trading WebSocket.
#
# Stack: transport → bpt-refdata → bpt-md-gateway + order-gateway → bpt-strategy
# Backtester is NOT started — the real exchange takes its place.
#
# Usage:
#   ./paper.sh start [bpt-strategy-config]   Start the paper trading stack.
#   ./paper.sh stop                    Stop all services.
#   ./paper.sh status                  Show running state.
#
# The optional bpt-strategy-config argument overrides the default strategy config.
# Default: bpt-strategy/config/vwap_reversion.qa-okx.toml

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

TRANSPORT_DIR="$STACK_DIR/transport/aeron"
REFDATA_DIR="$STACK_DIR/bpt-refdata"
MD_GATEWAY_DIR="$STACK_DIR/bpt-md-gateway"
ORDER_GATEWAY_DIR="$STACK_DIR/bpt-order-gateway"
PRICER_DIR="$STACK_DIR/bpt-pricer"
STRATEGY_DIR="$STACK_DIR/bpt-strategy"

STRATEGY_CONFIG="${2:-$STRATEGY_DIR/config/vwap_reversion.qa-okx.toml}"

# Auto-detect exchange from bpt-strategy config filename.
# e.g. short_vol.qa-deribit.toml → deribit, vwap_reversion.qa-okx.toml → okx
EXCHANGE="okx"
case "$STRATEGY_CONFIG" in
    *hyperliquid*) EXCHANGE="hyperliquid" ;;
    *deribit*)     EXCHANGE="deribit" ;;
    *okx*)         EXCHANGE="okx" ;;
    *binance*)     EXCHANGE="binance" ;;
esac

REFDATA_CONFIG="$REFDATA_DIR/config/bpt-refdata.qa-${EXCHANGE}.toml"
MD_GATEWAY_CONFIG="$MD_GATEWAY_DIR/config/bpt-md-gateway.qa-${EXCHANGE}.toml"
ORDER_GATEWAY_CONFIG="$ORDER_GATEWAY_DIR/config/order-gateway.qa-${EXCHANGE}.toml"
PRICER_CONFIG="$PRICER_DIR/config/bpt-pricer.qa-${EXCHANGE}.toml"

# Options strategies need bpt-pricer for vol surface computation.
NEEDS_PRICER=false
case "$STRATEGY_CONFIG" in
    *short_vol*|*vol*|*option*) NEEDS_PRICER=true ;;
esac

# Source exchange-specific credentials
CREDS_FILE="$HOME/.bpt-secrets/${EXCHANGE}-testnet.env"
if [ -f "$CREDS_FILE" ]; then
    # shellcheck disable=SC1090
    source "$CREDS_FILE"
fi

# ── Helpers ───────────────────────────────────────────────────────

is_running() {
    local pid_file="$1"
    [ -f "$pid_file" ] && kill -0 "$(cat "$pid_file")" 2>/dev/null
}

service_status() {
    local name="$1"
    local pid_file="$2"
    if is_running "$pid_file"; then
        echo "  [UP]   $name (PID $(cat "$pid_file"))"
    else
        echo "  [DOWN] $name"
    fi
}

do_status() {
    echo "Paper trading stack status ($EXCHANGE):"
    service_status "transport" "$TRANSPORT_DIR/.bifrost.pid"
    service_status "bpt-refdata"         "$REFDATA_DIR/.bpt-refdata.pid"
    service_status "bpt-md-gateway"         "$MD_GATEWAY_DIR/.bpt-md-gateway.pid"
    service_status "order-gateway"       "$ORDER_GATEWAY_DIR/.order-gateway.pid"
    $NEEDS_PRICER && service_status "bpt-pricer" "$PRICER_DIR/.bpt-pricer.pid"
    service_status "bpt-strategy"         "$STRATEGY_DIR/.bpt-strategy.pid"
}

do_start() {
    echo "=== Starting Strategy paper trading stack (${EXCHANGE} testnet) ==="
    echo "  Exchange      : $EXCHANGE"
    echo "  Strategy config : $STRATEGY_CONFIG"
    echo "  Refdata config : $REFDATA_CONFIG"
    echo "  Huginn config : $MD_GATEWAY_CONFIG"
    echo "  Heimdall cfg  : $ORDER_GATEWAY_CONFIG"
    $NEEDS_PRICER && echo "  Pricer config  : $PRICER_CONFIG"
    echo

    # 1. Bifrost-fabric — Aeron media driver first
    "$TRANSPORT_DIR/scripts/dev_start.sh"
    echo

    # 2. Refdata — reference data
    "$REFDATA_DIR/scripts/start.sh" "$REFDATA_CONFIG"
    echo

    # 3. Huginn + Heimdall (+ Pricer if options) — parallel startup.
    "$MD_GATEWAY_DIR/scripts/start.sh" "$MD_GATEWAY_CONFIG" &
    MD_GATEWAY_PID=$!

    "$ORDER_GATEWAY_DIR/scripts/start.sh" "$ORDER_GATEWAY_CONFIG" &
    ORDER_GATEWAY_PID=$!

    if $NEEDS_PRICER; then
        "$PRICER_DIR/scripts/start.sh" "$PRICER_CONFIG" &
        PRICER_PID=$!
    fi

    wait "$MD_GATEWAY_PID"
    wait "$ORDER_GATEWAY_PID"
    $NEEDS_PRICER && wait "$PRICER_PID"
    echo

    # 4. Strategy — waits for RefDataReady from Refdata, subscribes to MD via
    #    Huginn, and begins trading.
    "$STRATEGY_DIR/scripts/start.sh" "$STRATEGY_CONFIG"
    echo

    echo "=== Paper trading stack is up ==="
    echo
    do_status
    echo
    echo "Logs:"
    echo "  bpt-strategy  : tail -f $STRATEGY_DIR/logs/bpt-strategy.log"
    echo "  bpt-md-gateway  : tail -f $MD_GATEWAY_DIR/logs/bpt-md-gateway.log"
    echo "  order-gateway: tail -f $ORDER_GATEWAY_DIR/logs/order-gateway.log"
    if $NEEDS_PRICER; then
        echo "  bpt-pricer   : tail -f $PRICER_DIR/logs/bpt-pricer.log"
    fi
}

do_stop() {
    echo "=== Stopping paper trading stack ==="
    "$STRATEGY_DIR/scripts/stop.sh"      2>/dev/null || true
    $NEEDS_PRICER && "$PRICER_DIR/scripts/stop.sh" 2>/dev/null || true
    "$ORDER_GATEWAY_DIR/scripts/stop.sh"    2>/dev/null || true
    "$MD_GATEWAY_DIR/scripts/stop.sh"      2>/dev/null || true
    "$REFDATA_DIR/scripts/stop.sh"      2>/dev/null || true
    "$TRANSPORT_DIR/scripts/dev_stop.sh" 2>/dev/null || true
    echo "=== Paper trading stack is down ==="
}

case "${1:-}" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 {start|stop|status} [bpt-strategy-config]"
        exit 1
        ;;
esac

#!/bin/bash
# stack-testnet.sh — Start the Strategy stack in OKX demo trading mode.
#
# Usage:
#   ./stack-testnet.sh start   Start all services with testnet configs.
#   ./stack-testnet.sh stop    Stop all services.
#   ./stack-testnet.sh status  Show running state.

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

TRANSPORT_DIR="$STACK_DIR/transport/aeron"
REFDATA_DIR="$STACK_DIR/bpt-refdata"
MD_GATEWAY_DIR="$STACK_DIR/bpt-md-gateway"
ORDER_GATEWAY_DIR="$STACK_DIR/bpt-order-gateway"
STRATEGY_DIR="$STACK_DIR/bpt-strategy"

STRATEGY_CONFIG="$STRATEGY_DIR/config/vwap_reversion.qa-okx.toml"

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
    echo "Testnet stack status:"
    service_status "transport" "$TRANSPORT_DIR/.bifrost.pid"
    service_status "bpt-refdata"         "$REFDATA_DIR/.bpt-refdata.pid"
    service_status "bpt-md-gateway"         "$MD_GATEWAY_DIR/.bpt-md-gateway.pid"
    service_status "order-gateway"        "$ORDER_GATEWAY_DIR/.order-gateway.pid"
    service_status "bpt-strategy"         "$STRATEGY_DIR/.bpt-strategy.pid"
}

do_start() {
    echo "=== Starting Strategy stack (OKX demo / Avellaneda-Stoikov) ==="
    echo "  Strategy config : $STRATEGY_CONFIG"
    echo

    # 1. Bifrost-fabric
    "$TRANSPORT_DIR/scripts/dev_start.sh"
    echo

    # 2. Refdata — testnet config (OKX simulated, others disabled)
    "$REFDATA_DIR/scripts/start.sh" "$REFDATA_DIR/config/bpt-refdata.qa-okx.toml"
    echo

    # 3. Huginn + Heimdall in parallel — both use testnet configs by default
    "$MD_GATEWAY_DIR/scripts/start.sh" "$MD_GATEWAY_DIR/config/bpt-md-gateway.qa-okx.toml" &
    MD_GATEWAY_PID=$!

    "$ORDER_GATEWAY_DIR/scripts/start.sh" "$ORDER_GATEWAY_DIR/config/order-gateway.qa-okx.toml" &
    ORDER_GATEWAY_PID=$!

    wait "$MD_GATEWAY_PID"
    wait "$ORDER_GATEWAY_PID"
    echo

    # 4. Strategy — Avellaneda-Stoikov strategy, BTC/USDT:SPOT on OKX
    "$STRATEGY_DIR/scripts/start.sh" "$STRATEGY_CONFIG"
    echo

    echo "=== Testnet stack is up ==="
    echo
    do_status
    echo
    echo "Metrics:"
    echo "  transport : http://localhost:9100/metrics"
    echo "  bpt-refdata         : http://localhost:9101/metrics"
    echo "  bpt-md-gateway         : http://localhost:9102/metrics"
    echo "  order-gateway        : http://localhost:9103/metrics"
    echo "  bpt-strategy         : http://localhost:9104/metrics"
    echo
    echo "Logs:"
    echo "  tail -f $STRATEGY_DIR/logs/bpt-strategy.log"
    echo "  tail -f $ORDER_GATEWAY_DIR/logs/order-gateway.log"
}

do_stop() {
    echo "=== Stopping testnet stack ==="
    "$STRATEGY_DIR/scripts/stop.sh"
    "$ORDER_GATEWAY_DIR/scripts/stop.sh"
    "$MD_GATEWAY_DIR/scripts/stop.sh"
    "$REFDATA_DIR/scripts/stop.sh"
    "$TRANSPORT_DIR/scripts/dev_stop.sh"
    echo "=== Testnet stack is down ==="
}

case "${1:-}" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 {start|stop|status}"
        exit 1
        ;;
esac

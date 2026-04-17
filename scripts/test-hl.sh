#!/bin/bash
# test-hl.sh — Run the Strategy stack against Hyperliquid TESTNET with the
# Avellaneda-Stoikov market maker.
#
# No real money. Uses testnet USDC and qa-hyperliquid service configs.
#
# Usage:
#   ./test-hl.sh start      Start the testnet HL stack (AS strategy).
#   ./test-hl.sh stop       Stop all services.
#   ./test-hl.sh status     Show running state.

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

TRANSPORT_DIR="$STACK_DIR/transport/aeron"
REFDATA_DIR="$STACK_DIR/bpt-refdata"
MD_GATEWAY_DIR="$STACK_DIR/bpt-md-gateway"
ORDER_GATEWAY_DIR="$STACK_DIR/bpt-order-gateway"
STRATEGY_DIR="$STACK_DIR/bpt-strategy"

STRATEGY_CONFIG="$STRATEGY_DIR/config/avellaneda_stoikov.qa-hyperliquid.toml"
REFDATA_CONFIG="$REFDATA_DIR/config/bpt-refdata.qa-hyperliquid.toml"
MD_GATEWAY_CONFIG="$MD_GATEWAY_DIR/config/bpt-md-gateway.qa-hyperliquid.toml"
ORDER_GATEWAY_CONFIG="$ORDER_GATEWAY_DIR/config/order-gateway.qa-hyperliquid.toml"

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
    echo "TEST-HL stack status:"
    service_status "transport" "$TRANSPORT_DIR/.bifrost.pid"
    service_status "bpt-refdata"         "$REFDATA_DIR/.bpt-refdata.pid"
    service_status "bpt-md-gateway"         "$MD_GATEWAY_DIR/.bpt-md-gateway.pid"
    service_status "order-gateway"       "$ORDER_GATEWAY_DIR/.order-gateway.pid"
    service_status "bpt-strategy"         "$STRATEGY_DIR/.bpt-strategy.pid"
}

check_preflight() {
    local missing=()
    [ -f "$REFDATA_CONFIG"   ] || missing+=("$REFDATA_CONFIG")
    [ -f "$MD_GATEWAY_CONFIG"   ] || missing+=("$MD_GATEWAY_CONFIG")
    [ -f "$ORDER_GATEWAY_CONFIG" ] || missing+=("$ORDER_GATEWAY_CONFIG")
    [ -f "$STRATEGY_CONFIG"   ] || missing+=("$STRATEGY_CONFIG")

    if [ ${#missing[@]} -gt 0 ]; then
        echo "ERROR: cannot start TEST-HL stack — missing:"
        for m in "${missing[@]}"; do echo "  - $m"; done
        exit 1
    fi
}

do_start() {
    check_preflight

    echo "=== Starting Strategy TESTNET stack (Hyperliquid testnet / AS) ==="
    echo "  Strategy     : Avellaneda-Stoikov market maker"
    echo "  Instrument   : BTC/USD:PERPETUAL @ HYPERLIQUID testnet"
    echo "  Strategy cfg   : $STRATEGY_CONFIG"
    echo

    "$TRANSPORT_DIR/scripts/dev_start.sh"
    echo
    "$REFDATA_DIR/scripts/start.sh" "$REFDATA_CONFIG"
    echo

    "$MD_GATEWAY_DIR/scripts/start.sh" "$MD_GATEWAY_CONFIG" &
    MD_GATEWAY_PID=$!
    "$ORDER_GATEWAY_DIR/scripts/start.sh" "$ORDER_GATEWAY_CONFIG" &
    ORDER_GATEWAY_PID=$!
    wait "$MD_GATEWAY_PID"
    wait "$ORDER_GATEWAY_PID"
    echo

    "$STRATEGY_DIR/scripts/start.sh" "$STRATEGY_CONFIG"
    echo

    echo "=== TEST-HL stack is up ==="
    echo
    do_status
    echo
    echo "Metrics:"
    echo "  transport : http://localhost:9100/metrics"
    echo "  bpt-refdata         : http://localhost:9101/metrics"
    echo "  bpt-md-gateway         : http://localhost:9102/metrics"
    echo "  order-gateway       : http://localhost:9103/metrics"
    echo "  bpt-strategy         : http://localhost:9104/metrics"
    echo
    echo "Stop with: $0 stop"
    echo "Tail logs: tail -f $STRATEGY_DIR/logs/bpt-strategy.log"
}

do_stop() {
    echo "=== Stopping TEST-HL stack ==="
    "$STRATEGY_DIR/scripts/stop.sh"      2>/dev/null || true
    "$ORDER_GATEWAY_DIR/scripts/stop.sh"    2>/dev/null || true
    "$MD_GATEWAY_DIR/scripts/stop.sh"      2>/dev/null || true
    "$REFDATA_DIR/scripts/stop.sh"      2>/dev/null || true
    "$TRANSPORT_DIR/scripts/dev_stop.sh" 2>/dev/null || true
    echo "=== TEST-HL stack is down ==="
}

case "${1:-}" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 start   (starts AS on HL testnet)"
        echo "       $0 stop"
        echo "       $0 status"
        exit 1
        ;;
esac

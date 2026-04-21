#!/bin/bash
# live.sh — Run the Strategy LIVE trading stack against OKX mainnet.
#
#   ██╗     ██╗██╗   ██╗███████╗
#   ██║     ██║██║   ██║██╔════╝
#   ██║     ██║██║   ██║█████╗
#   ██║     ██║╚██╗ ██╔╝██╔══╝
#   ███████╗██║ ╚████╔╝ ███████╗
#   ╚══════╝╚═╝  ╚═══╝  ╚══════╝
#
# This script places REAL orders with REAL money against the exchange's
# mainnet endpoints.  Fills will settle on-chain / on-exchange and cannot
# be reversed.
#
# Before first use, create these config files pointing at mainnet endpoints
# and live credentials:
#
#   bpt-refdata/config/bpt-refdata.live-okx.toml
#   bpt-md-gateway/config/bpt-md-gateway.live-okx.toml
#   order-gateway/config/order-gateway.live-okx.toml
#   bpt-strategy/config/<strategy>.live-okx.toml
#
# Usage:
#   ./live.sh start <bpt-strategy-config>   REQUIRED — no default, explicit only.
#   ./live.sh stop                    Stop all services.
#   ./live.sh status                  Show running state.

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

TRANSPORT_DIR="$STACK_DIR/transport/aeron"
REFDATA_DIR="$STACK_DIR/bpt-refdata"
MD_GATEWAY_DIR="$STACK_DIR/bpt-md-gateway"
ORDER_GATEWAY_DIR="$STACK_DIR/bpt-order-gateway"
STRATEGY_DIR="$STACK_DIR/bpt-strategy"

STRATEGY_CONFIG="${2:-}"  # no default — live trading requires explicit choice
REFDATA_CONFIG="$REFDATA_DIR/config/bpt-refdata.live-okx.toml"
MD_GATEWAY_CONFIG="$MD_GATEWAY_DIR/config/bpt-md-gateway.live-okx.toml"
ORDER_GATEWAY_CONFIG="$ORDER_GATEWAY_DIR/config/order-gateway.live-okx.toml"

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
    echo "LIVE trading stack status:"
    service_status "transport" "$TRANSPORT_DIR/.bpt-transport.pid"
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
    [ -z "$STRATEGY_CONFIG"   ] && missing+=("<bpt-strategy-config arg>")
    [ -n "$STRATEGY_CONFIG" ] && [ ! -f "$STRATEGY_CONFIG" ] && missing+=("$STRATEGY_CONFIG")

    if [ ${#missing[@]} -gt 0 ]; then
        echo "ERROR: cannot start LIVE stack — missing:"
        for m in "${missing[@]}"; do echo "  - $m"; done
        echo
        echo "Live trading requires explicit config files for every service."
        echo "Copy the qa-okx variants and edit them to point at mainnet endpoints"
        echo "and live credentials before using this script."
        exit 1
    fi
}

confirm() {
    echo
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  YOU ARE ABOUT TO START LIVE TRADING AGAINST OKX MAINNET    ║"
    echo "║  Real orders.  Real money.  Fills cannot be reversed.       ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo
    echo "  Strategy config : $STRATEGY_CONFIG"
    echo
    read -r -p "Type 'I UNDERSTAND' to continue, anything else to abort: " reply
    if [ "$reply" != "I UNDERSTAND" ]; then
        echo "Aborted."
        exit 1
    fi
}

do_start() {
    check_preflight
    confirm

    echo "=== Starting Strategy LIVE trading stack (OKX mainnet) ==="
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

    echo "=== LIVE trading stack is up — Strategy is trading with real money ==="
    echo
    do_status
    echo
    echo "Stop with: $0 stop"
}

do_stop() {
    echo "=== Stopping LIVE trading stack ==="
    "$STRATEGY_DIR/scripts/stop.sh"      2>/dev/null || true
    "$ORDER_GATEWAY_DIR/scripts/stop.sh"    2>/dev/null || true
    "$MD_GATEWAY_DIR/scripts/stop.sh"      2>/dev/null || true
    "$REFDATA_DIR/scripts/stop.sh"      2>/dev/null || true
    "$TRANSPORT_DIR/scripts/dev_stop.sh" 2>/dev/null || true
    echo "=== LIVE trading stack is down ==="
}

case "${1:-}" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 start <bpt-strategy-config>"
        echo "       $0 stop"
        echo "       $0 status"
        exit 1
        ;;
esac

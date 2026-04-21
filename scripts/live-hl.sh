#!/bin/bash
# live-hl.sh — Run the Strategy LIVE trading stack against Hyperliquid mainnet.
#
#   ██╗     ██╗██╗   ██╗███████╗    ██╗  ██╗██╗
#   ██║     ██║██║   ██║██╔════╝    ██║  ██║██║
#   ██║     ██║██║   ██║█████╗      ███████║██║
#   ██║     ██║╚██╗ ██╔╝██╔══╝      ██╔══██║██║
#   ███████╗██║ ╚████╔╝ ███████╗    ██║  ██║███████╗
#   ╚══════╝╚═╝  ╚═══╝  ╚══════╝    ╚═╝  ╚═╝╚══════╝
#
# This script places REAL orders with REAL money on Hyperliquid mainnet.
# Fills settle on-chain and cannot be reversed.
#
# Prerequisites (do these once, before first run):
#   1. Fund a Hyperliquid mainnet wallet with USDC collateral.
#   2. Create credentials at the path expected by order-gateway's HL adapter.
#      The service configs reference secret_name "bpt/prod/HYPERLIQUID"
#      (see bpt-refdata/config/exchanges/live.toml). Local dev mode loads
#      secrets from ~/.bpt-secrets/ — inspect that directory's existing
#      testnet variants for the exact filename + schema before filling.
#   3. Verify these configs all exist and point at mainnet:
#        bpt-refdata/config/bpt-refdata.hyperliquid.toml
#        bpt-md-gateway/config/bpt-md-gateway.hyperliquid.toml
#        order-gateway/config/order-gateway.hyperliquid.toml
#        bpt-strategy/config/ofi.hl-live.toml
#
# Usage:
#   ./live-hl.sh start      Start the live HL stack (OFI strategy).
#   ./live-hl.sh stop       Stop all services.
#   ./live-hl.sh status     Show running state.

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

TRANSPORT_DIR="$STACK_DIR/transport/aeron"
REFDATA_DIR="$STACK_DIR/bpt-refdata"
MD_GATEWAY_DIR="$STACK_DIR/bpt-md-gateway"
ORDER_GATEWAY_DIR="$STACK_DIR/bpt-order-gateway"
STRATEGY_DIR="$STACK_DIR/bpt-strategy"

STRATEGY_CONFIG="$STRATEGY_DIR/config/ofi.hl-live.toml"
REFDATA_CONFIG="$REFDATA_DIR/config/bpt-refdata.hyperliquid.toml"
MD_GATEWAY_CONFIG="$MD_GATEWAY_DIR/config/bpt-md-gateway.hyperliquid.toml"
ORDER_GATEWAY_CONFIG="$ORDER_GATEWAY_DIR/config/order-gateway.hyperliquid.toml"

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
    echo "LIVE-HL trading stack status:"
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
    [ -f "$STRATEGY_CONFIG"   ] || missing+=("$STRATEGY_CONFIG")

    if [ ${#missing[@]} -gt 0 ]; then
        echo "ERROR: cannot start LIVE-HL stack — missing:"
        for m in "${missing[@]}"; do echo "  - $m"; done
        exit 1
    fi
}

confirm() {
    echo
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║  YOU ARE ABOUT TO START LIVE TRADING ON HYPERLIQUID MAINNET ║"
    echo "║  Real orders.  Real USDC.  Fills cannot be reversed.        ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo
    echo "  Strategy     : OFI (taker IOC, v3: time-stop only)"
    echo "  Instrument   : BTC/USD:PERPETUAL @ HYPERLIQUID"
    echo "  Strategy cfg   : $STRATEGY_CONFIG"
    echo "  Risk caps    : qty_usd=10, max_position=50, max_daily_loss=20"
    echo "  Expected cost: \$5-15 of information-buying on the first 1-2 hours"
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

    echo "=== Starting Strategy LIVE trading stack (Hyperliquid mainnet) ==="
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

    echo "=== LIVE-HL trading stack is up — Strategy is trading with real USDC ==="
    echo
    do_status
    echo
    echo "Stop with: $0 stop"
    echo "Tail logs: tail -f $STRATEGY_DIR/logs/bpt-strategy.log"
    echo "Analysis : python3 $STRATEGY_DIR/scripts/analyze_ofi_markouts.py"
}

do_stop() {
    echo "=== Stopping LIVE-HL trading stack ==="
    "$STRATEGY_DIR/scripts/stop.sh"      2>/dev/null || true
    "$ORDER_GATEWAY_DIR/scripts/stop.sh"    2>/dev/null || true
    "$MD_GATEWAY_DIR/scripts/stop.sh"      2>/dev/null || true
    "$REFDATA_DIR/scripts/stop.sh"      2>/dev/null || true
    "$TRANSPORT_DIR/scripts/dev_stop.sh" 2>/dev/null || true
    echo "=== LIVE-HL trading stack is down ==="
}

case "${1:-}" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 start   (starts OFI on HL mainnet)"
        echo "       $0 stop"
        echo "       $0 status"
        exit 1
        ;;
esac

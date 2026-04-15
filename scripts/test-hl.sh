#!/bin/bash
# test-hl.sh — Run the Fenrir stack against Hyperliquid TESTNET with the
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

BIFROST_DIR="$STACK_DIR/bifrost/fabric"
MUNINN_DIR="$STACK_DIR/muninn"
HUGINN_DIR="$STACK_DIR/huginn"
HEIMDALL_DIR="$STACK_DIR/heimdall"
FENRIR_DIR="$STACK_DIR/fenrir"

FENRIR_CONFIG="$FENRIR_DIR/config/avellaneda_stoikov.qa-hyperliquid.toml"
MUNINN_CONFIG="$MUNINN_DIR/config/muninn.qa-hyperliquid.toml"
HUGINN_CONFIG="$HUGINN_DIR/config/huginn.qa-hyperliquid.toml"
HEIMDALL_CONFIG="$HEIMDALL_DIR/config/heimdall.qa-hyperliquid.toml"

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
    service_status "bifrost-fabric" "$BIFROST_DIR/.bifrost.pid"
    service_status "muninn"         "$MUNINN_DIR/.muninn.pid"
    service_status "huginn"         "$HUGINN_DIR/.huginn.pid"
    service_status "heimdall"       "$HEIMDALL_DIR/.heimdall.pid"
    service_status "fenrir"         "$FENRIR_DIR/.fenrir.pid"
}

check_preflight() {
    local missing=()
    [ -f "$MUNINN_CONFIG"   ] || missing+=("$MUNINN_CONFIG")
    [ -f "$HUGINN_CONFIG"   ] || missing+=("$HUGINN_CONFIG")
    [ -f "$HEIMDALL_CONFIG" ] || missing+=("$HEIMDALL_CONFIG")
    [ -f "$FENRIR_CONFIG"   ] || missing+=("$FENRIR_CONFIG")

    if [ ${#missing[@]} -gt 0 ]; then
        echo "ERROR: cannot start TEST-HL stack — missing:"
        for m in "${missing[@]}"; do echo "  - $m"; done
        exit 1
    fi
}

do_start() {
    check_preflight

    echo "=== Starting Fenrir TESTNET stack (Hyperliquid testnet / AS) ==="
    echo "  Strategy     : Avellaneda-Stoikov market maker"
    echo "  Instrument   : BTC/USD:PERPETUAL @ HYPERLIQUID testnet"
    echo "  Fenrir cfg   : $FENRIR_CONFIG"
    echo

    "$BIFROST_DIR/scripts/dev_start.sh"
    echo
    "$MUNINN_DIR/scripts/start.sh" "$MUNINN_CONFIG"
    echo

    "$HUGINN_DIR/scripts/start.sh" "$HUGINN_CONFIG" &
    HUGINN_PID=$!
    "$HEIMDALL_DIR/scripts/start.sh" "$HEIMDALL_CONFIG" &
    HEIMDALL_PID=$!
    wait "$HUGINN_PID"
    wait "$HEIMDALL_PID"
    echo

    "$FENRIR_DIR/scripts/start.sh" "$FENRIR_CONFIG"
    echo

    echo "=== TEST-HL stack is up ==="
    echo
    do_status
    echo
    echo "Metrics:"
    echo "  bifrost-fabric : http://localhost:9100/metrics"
    echo "  muninn         : http://localhost:9101/metrics"
    echo "  huginn         : http://localhost:9102/metrics"
    echo "  heimdall       : http://localhost:9103/metrics"
    echo "  fenrir         : http://localhost:9104/metrics"
    echo
    echo "Stop with: $0 stop"
    echo "Tail logs: tail -f $FENRIR_DIR/logs/fenrir.log"
}

do_stop() {
    echo "=== Stopping TEST-HL stack ==="
    "$FENRIR_DIR/scripts/stop.sh"      2>/dev/null || true
    "$HEIMDALL_DIR/scripts/stop.sh"    2>/dev/null || true
    "$HUGINN_DIR/scripts/stop.sh"      2>/dev/null || true
    "$MUNINN_DIR/scripts/stop.sh"      2>/dev/null || true
    "$BIFROST_DIR/scripts/dev_stop.sh" 2>/dev/null || true
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

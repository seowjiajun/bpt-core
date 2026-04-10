#!/bin/bash
# stack-testnet.sh — Start the Fenrir stack in OKX demo trading mode.
#
# Usage:
#   ./stack-testnet.sh start   Start all services with testnet configs.
#   ./stack-testnet.sh stop    Stop all services.
#   ./stack-testnet.sh status  Show running state.

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

BIFROST_DIR="$STACK_DIR/bifrost/fabric"
MUNINN_DIR="$STACK_DIR/muninn"
HUGINN_DIR="$STACK_DIR/huginn"
HEIMDALL_DIR="$STACK_DIR/heimdall"
FENRIR_DIR="$STACK_DIR/fenrir"

FENRIR_CONFIG="$FENRIR_DIR/config/vwap_reversion.qa-okx.toml"

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
    service_status "bifrost-fabric" "$BIFROST_DIR/.bifrost.pid"
    service_status "muninn"         "$MUNINN_DIR/.muninn.pid"
    service_status "huginn"         "$HUGINN_DIR/.huginn.pid"
    service_status "heimdall"        "$HEIMDALL_DIR/.heimdall.pid"
    service_status "fenrir"         "$FENRIR_DIR/.fenrir.pid"
}

do_start() {
    echo "=== Starting Fenrir stack (OKX demo / Avellaneda-Stoikov) ==="
    echo "  Fenrir config : $FENRIR_CONFIG"
    echo

    # 1. Bifrost-fabric
    "$BIFROST_DIR/scripts/dev_start.sh"
    echo

    # 2. Muninn — testnet config (OKX simulated, others disabled)
    "$MUNINN_DIR/scripts/start.sh" "$MUNINN_DIR/config/muninn.qa-okx.toml"
    echo

    # 3. Huginn + Heimdall in parallel — both use testnet configs by default
    "$HUGINN_DIR/scripts/start.sh" "$HUGINN_DIR/config/huginn.qa-okx.toml" &
    HUGINN_PID=$!

    "$HEIMDALL_DIR/scripts/start.sh" "$HEIMDALL_DIR/config/heimdall.qa-okx.toml" &
    HEIMDALL_PID=$!

    wait "$HUGINN_PID"
    wait "$HEIMDALL_PID"
    echo

    # 4. Fenrir — Avellaneda-Stoikov strategy, BTC/USDT:SPOT on OKX
    "$FENRIR_DIR/scripts/start.sh" "$FENRIR_CONFIG"
    echo

    echo "=== Testnet stack is up ==="
    echo
    do_status
    echo
    echo "Metrics:"
    echo "  bifrost-fabric : http://localhost:9100/metrics"
    echo "  muninn         : http://localhost:9101/metrics"
    echo "  huginn         : http://localhost:9102/metrics"
    echo "  heimdall        : http://localhost:9103/metrics"
    echo "  fenrir         : http://localhost:9104/metrics"
    echo
    echo "Logs:"
    echo "  tail -f $FENRIR_DIR/logs/fenrir.log"
    echo "  tail -f $HEIMDALL_DIR/logs/heimdall.log"
}

do_stop() {
    echo "=== Stopping testnet stack ==="
    "$FENRIR_DIR/scripts/stop.sh"
    "$HEIMDALL_DIR/scripts/stop.sh"
    "$HUGINN_DIR/scripts/stop.sh"
    "$MUNINN_DIR/scripts/stop.sh"
    "$BIFROST_DIR/scripts/dev_stop.sh"
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

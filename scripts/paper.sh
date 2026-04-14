#!/bin/bash
# paper.sh — Run the Fenrir paper trading stack against OKX testnet.
#
# Paper trading uses real market data from the exchange's testnet endpoints
# with simulated fills against a testnet account.  No Jormungandr — data
# comes straight from OKX's demo-trading WebSocket.
#
# Stack: bifrost-fabric → muninn → huginn + heimdall → fenrir
# Jormungandr is NOT started — the real exchange takes its place.
#
# Usage:
#   ./paper.sh start [fenrir-config]   Start the paper trading stack.
#   ./paper.sh stop                    Stop all services.
#   ./paper.sh status                  Show running state.
#
# The optional fenrir-config argument overrides the default strategy config.
# Default: fenrir/config/vwap_reversion.qa-okx.toml

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

BIFROST_DIR="$STACK_DIR/bifrost/fabric"
MUNINN_DIR="$STACK_DIR/muninn"
HUGINN_DIR="$STACK_DIR/huginn"
HEIMDALL_DIR="$STACK_DIR/heimdall"
SURTR_DIR="$STACK_DIR/surtr"
FENRIR_DIR="$STACK_DIR/fenrir"

FENRIR_CONFIG="${2:-$FENRIR_DIR/config/vwap_reversion.qa-okx.toml}"

# Auto-detect exchange from fenrir config filename.
# e.g. short_vol.qa-deribit.toml → deribit, vwap_reversion.qa-okx.toml → okx
EXCHANGE="okx"
case "$FENRIR_CONFIG" in
    *hyperliquid*) EXCHANGE="hyperliquid" ;;
    *deribit*)     EXCHANGE="deribit" ;;
    *okx*)         EXCHANGE="okx" ;;
    *binance*)     EXCHANGE="binance" ;;
esac

MUNINN_CONFIG="$MUNINN_DIR/config/muninn.qa-${EXCHANGE}.toml"
HUGINN_CONFIG="$HUGINN_DIR/config/huginn.qa-${EXCHANGE}.toml"
HEIMDALL_CONFIG="$HEIMDALL_DIR/config/heimdall.qa-${EXCHANGE}.toml"
SURTR_CONFIG="$SURTR_DIR/config/surtr.qa-${EXCHANGE}.toml"

# Options strategies need surtr for vol surface computation.
NEEDS_SURTR=false
case "$FENRIR_CONFIG" in
    *short_vol*|*vol*|*option*) NEEDS_SURTR=true ;;
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
    service_status "bifrost-fabric" "$BIFROST_DIR/.bifrost.pid"
    service_status "muninn"         "$MUNINN_DIR/.muninn.pid"
    service_status "huginn"         "$HUGINN_DIR/.huginn.pid"
    service_status "heimdall"       "$HEIMDALL_DIR/.heimdall.pid"
    $NEEDS_SURTR && service_status "surtr" "$SURTR_DIR/.surtr.pid"
    service_status "fenrir"         "$FENRIR_DIR/.fenrir.pid"
}

do_start() {
    echo "=== Starting Fenrir paper trading stack (${EXCHANGE} testnet) ==="
    echo "  Exchange      : $EXCHANGE"
    echo "  Fenrir config : $FENRIR_CONFIG"
    echo "  Muninn config : $MUNINN_CONFIG"
    echo "  Huginn config : $HUGINN_CONFIG"
    echo "  Heimdall cfg  : $HEIMDALL_CONFIG"
    $NEEDS_SURTR && echo "  Surtr config  : $SURTR_CONFIG"
    echo

    # 1. Bifrost-fabric — Aeron media driver first
    "$BIFROST_DIR/scripts/dev_start.sh"
    echo

    # 2. Muninn — reference data
    "$MUNINN_DIR/scripts/start.sh" "$MUNINN_CONFIG"
    echo

    # 3. Huginn + Heimdall (+ Surtr if options) — parallel startup.
    "$HUGINN_DIR/scripts/start.sh" "$HUGINN_CONFIG" &
    HUGINN_PID=$!

    "$HEIMDALL_DIR/scripts/start.sh" "$HEIMDALL_CONFIG" &
    HEIMDALL_PID=$!

    if $NEEDS_SURTR; then
        "$SURTR_DIR/scripts/start.sh" "$SURTR_CONFIG" &
        SURTR_PID=$!
    fi

    wait "$HUGINN_PID"
    wait "$HEIMDALL_PID"
    $NEEDS_SURTR && wait "$SURTR_PID"
    echo

    # 4. Fenrir — waits for RefDataReady from Muninn, subscribes to MD via
    #    Huginn, and begins trading.
    "$FENRIR_DIR/scripts/start.sh" "$FENRIR_CONFIG"
    echo

    echo "=== Paper trading stack is up ==="
    echo
    do_status
    echo
    echo "Logs:"
    echo "  fenrir  : tail -f $FENRIR_DIR/logs/fenrir.log"
    echo "  huginn  : tail -f $HUGINN_DIR/logs/huginn.log"
    echo "  heimdall: tail -f $HEIMDALL_DIR/logs/heimdall.log"
    if $NEEDS_SURTR; then
        echo "  surtr   : tail -f $SURTR_DIR/logs/surtr.log"
    fi
}

do_stop() {
    echo "=== Stopping paper trading stack ==="
    "$FENRIR_DIR/scripts/stop.sh"      2>/dev/null || true
    $NEEDS_SURTR && "$SURTR_DIR/scripts/stop.sh" 2>/dev/null || true
    "$HEIMDALL_DIR/scripts/stop.sh"    2>/dev/null || true
    "$HUGINN_DIR/scripts/stop.sh"      2>/dev/null || true
    "$MUNINN_DIR/scripts/stop.sh"      2>/dev/null || true
    "$BIFROST_DIR/scripts/dev_stop.sh" 2>/dev/null || true
    echo "=== Paper trading stack is down ==="
}

case "${1:-}" in
    start)  do_start ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 {start|stop|status} [fenrir-config]"
        exit 1
        ;;
esac

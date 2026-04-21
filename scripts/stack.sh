#!/bin/bash
# stack.sh — Start or stop the full Strategy trading stack.
#
# Usage:
#   ./stack.sh start [bpt-strategy-config]   Start all services in order.
#                                      bpt-strategy-config defaults to config/momentum.yaml.
#   ./stack.sh stop                    Stop all services in reverse order.
#   ./stack.sh status                  Show running/stopped state of each service.
#
# Startup order:  transport → sindri → bpt-md-gateway + order-gateway → bpt-strategy
# Shutdown order: bpt-strategy → order-gateway → bpt-md-gateway → sindri → transport

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

TRANSPORT_DIR="$STACK_DIR/transport/aeron"
REFDATA_DIR="$STACK_DIR/bpt-refdata"
MD_GATEWAY_DIR="$STACK_DIR/bpt-md-gateway"
ORDER_GATEWAY_DIR="$STACK_DIR/bpt-order-gateway"
STRATEGY_DIR="$STACK_DIR/bpt-strategy"

cmd="${1:-}"

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

# ── Status ────────────────────────────────────────────────────────

do_status() {
    echo "Stack status:"
    service_status "transport" "$TRANSPORT_DIR/.bpt-transport.pid"
    service_status "bpt-refdata"         "$REFDATA_DIR/.bpt-refdata.pid"
    service_status "bpt-md-gateway"         "$MD_GATEWAY_DIR/.bpt-md-gateway.pid"
    service_status "order-gateway"        "$ORDER_GATEWAY_DIR/.order-gateway.pid"
    service_status "bpt-strategy"         "$STRATEGY_DIR/.bpt-strategy.pid"
}

# ── Start ─────────────────────────────────────────────────────────

do_start() {
    local bpt-strategy_config="${2:-$STRATEGY_DIR/config/momentum.yaml}"

    echo "=== Starting Strategy stack ==="
    echo

    # 1. bpt-transport (Aeron media driver)
    "$TRANSPORT_DIR/scripts/dev_start.sh"
    echo

    # 2. Refdata (refdata — must be ready before bpt-strategy)
    "$REFDATA_DIR/scripts/start.sh"
    echo

    # 3. Huginn + Heimdall (can start in parallel — neither depends on the other)
    "$MD_GATEWAY_DIR/scripts/start.sh" &
    MD_GATEWAY_START_PID=$!

    "$ORDER_GATEWAY_DIR/scripts/start.sh" &
    ORDER_GATEWAY_START_PID=$!

    wait "$MD_GATEWAY_START_PID"
    wait "$ORDER_GATEWAY_START_PID"
    echo

    # 4. Strategy (strategy engine — last, depends on all above)
    "$STRATEGY_DIR/scripts/start.sh" "$bpt-strategy_config"
    echo

    echo "=== Stack is up ==="
    echo
    do_status
}

# ── Stop ──────────────────────────────────────────────────────────

do_stop() {
    echo "=== Stopping Strategy stack ==="
    echo

    # Reverse order: bpt-strategy first, bpt-transport last
    "$STRATEGY_DIR/scripts/stop.sh"
    "$ORDER_GATEWAY_DIR/scripts/stop.sh"
    "$MD_GATEWAY_DIR/scripts/stop.sh"
    "$REFDATA_DIR/scripts/stop.sh" || true
    "$TRANSPORT_DIR/scripts/dev_stop.sh"

    echo
    echo "=== Stack is down ==="
}

# ── Dispatch ──────────────────────────────────────────────────────

case "$cmd" in
    start)  do_start "$@" ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 {start [bpt-strategy-config]|stop|status}"
        exit 1
        ;;
esac

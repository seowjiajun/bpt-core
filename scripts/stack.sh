#!/bin/bash
# stack.sh — Start or stop the full Fenrir trading stack.
#
# Usage:
#   ./stack.sh start [fenrir-config]   Start all services in order.
#                                      fenrir-config defaults to config/momentum.yaml.
#   ./stack.sh stop                    Stop all services in reverse order.
#   ./stack.sh status                  Show running/stopped state of each service.
#
# Startup order:  bifrost-fabric → sindri → huginn + heimdall → fenrir
# Shutdown order: fenrir → heimdall → huginn → sindri → bifrost-fabric

set -euo pipefail

STACK_DIR="$(cd "$(dirname "$0")/.." && pwd)"

BIFROST_DIR="$STACK_DIR/bifrost/fabric"
MUNINN_DIR="$STACK_DIR/muninn"
HUGINN_DIR="$STACK_DIR/huginn"
HEIMDALL_DIR="$STACK_DIR/heimdall"
FENRIR_DIR="$STACK_DIR/fenrir"

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
    service_status "bifrost-fabric" "$BIFROST_DIR/.bifrost.pid"
    service_status "muninn"         "$MUNINN_DIR/.muninn.pid"
    service_status "huginn"         "$HUGINN_DIR/.huginn.pid"
    service_status "heimdall"        "$HEIMDALL_DIR/.heimdall.pid"
    service_status "fenrir"         "$FENRIR_DIR/.fenrir.pid"
}

# ── Start ─────────────────────────────────────────────────────────

do_start() {
    local fenrir_config="${2:-$FENRIR_DIR/config/momentum.yaml}"

    echo "=== Starting Fenrir stack ==="
    echo

    # 1. Bifrost-fabric (Aeron media driver)
    "$BIFROST_DIR/scripts/dev_start.sh"
    echo

    # 2. Muninn (refdata — must be ready before fenrir)
    "$MUNINN_DIR/scripts/start.sh"
    echo

    # 3. Huginn + Heimdall (can start in parallel — neither depends on the other)
    "$HUGINN_DIR/scripts/start.sh" &
    HUGINN_START_PID=$!

    "$HEIMDALL_DIR/scripts/start.sh" &
    HEIMDALL_START_PID=$!

    wait "$HUGINN_START_PID"
    wait "$HEIMDALL_START_PID"
    echo

    # 4. Fenrir (strategy engine — last, depends on all above)
    "$FENRIR_DIR/scripts/start.sh" "$fenrir_config"
    echo

    echo "=== Stack is up ==="
    echo
    do_status
}

# ── Stop ──────────────────────────────────────────────────────────

do_stop() {
    echo "=== Stopping Fenrir stack ==="
    echo

    # Reverse order: fenrir first, bifrost last
    "$FENRIR_DIR/scripts/stop.sh"
    "$HEIMDALL_DIR/scripts/stop.sh"
    "$HUGINN_DIR/scripts/stop.sh"
    "$MUNINN_DIR/scripts/stop.sh" || true
    "$BIFROST_DIR/scripts/dev_stop.sh"

    echo
    echo "=== Stack is down ==="
}

# ── Dispatch ──────────────────────────────────────────────────────

case "$cmd" in
    start)  do_start "$@" ;;
    stop)   do_stop ;;
    status) do_status ;;
    *)
        echo "Usage: $0 {start [fenrir-config]|stop|status}"
        exit 1
        ;;
esac

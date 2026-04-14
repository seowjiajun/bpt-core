#!/bin/bash
# down.sh — tear down the BPT development stack.
#
# By default: stops the trading stack and bridge, leaves the monitoring
# stack (Prometheus + Grafana) running so you can inspect post-mortem
# metrics. Monitoring is treated like a prod always-on service.
#
# Usage:
#   ./scripts/down.sh                      # leave monitoring up
#   ./scripts/down.sh --with-monitoring    # take everything down

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WITH_MONITORING=0

while [ $# -gt 0 ]; do
    case "$1" in
        --with-monitoring) WITH_MONITORING=1; shift ;;
        *) echo "Unknown flag: $1"; exit 1 ;;
    esac
done

# ── Trading stack + bridge ──────────────────────────────────────────────
"$ROOT/dashboard/scripts/paper_run.sh" stop

# ── Monitoring (optional) ───────────────────────────────────────────────
if [ "$WITH_MONITORING" -eq 1 ]; then
    echo
    echo "=== Monitoring stack ==="
    (cd "$ROOT/monitoring" && make down)
else
    echo
    echo "Monitoring stack still running (Grafana: http://localhost:3000)."
    echo "To take it down: $0 --with-monitoring"
fi

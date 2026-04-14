#!/bin/bash
# up.sh — single entry point to bring up the full BPT development stack.
#
# Starts:
#   1. Monitoring stack (Prometheus + Grafana via docker compose)
#   2. Trading stack (bifrost-fabric, muninn, huginn, heimdall, fenrir)
#   3. Dashboard bridge (WebSocket :8080)
#
# NOT started — dev-only surfaces you run by hand:
#   - vite frontend (cd dashboard/frontend && npm run dev)
#
# Usage:
#   ./scripts/up.sh                                        # default Hyperliquid stoikov
#   ./scripts/up.sh fenrir/config/<strategy>.qa-<venue>.toml
#   ./scripts/up.sh ... --no-monitoring                    # skip Grafana
#
# Monitoring is intentionally treated like a prod always-on service:
# `./scripts/down.sh` does NOT stop it by default. To stop it, either
# `./scripts/down.sh --with-monitoring` or `cd monitoring && make down`.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_CFG="$ROOT/fenrir/config/avellaneda_stoikov.qa-hyperliquid.toml"

SKIP_MONITORING=0
CFG=""
EXTRA_ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --no-monitoring) SKIP_MONITORING=1; shift ;;
        --instrument-id|--instrument-id=*) EXTRA_ARGS+=("$1"); shift ;;
        -*) echo "Unknown flag: $1"; exit 1 ;;
        *)
            if [ -z "$CFG" ]; then CFG="$1"; fi
            shift ;;
    esac
done
CFG="${CFG:-$DEFAULT_CFG}"

if [ ! -f "$CFG" ]; then
    echo "ERROR: fenrir config not found: $CFG"
    exit 1
fi

# ── Monitoring stack (idempotent — compose handles already-up) ──────────
if [ "$SKIP_MONITORING" -eq 0 ]; then
    echo "=== Monitoring stack ==="
    (cd "$ROOT/monitoring" && make up 2>&1 | tail -6)
    echo
fi

# ── Trading stack + bridge ──────────────────────────────────────────────
export BPT_ENV="${BPT_ENV:-qa}"
"$ROOT/dashboard/scripts/paper_run.sh" start "$CFG" "${EXTRA_ARGS[@]}"

cat <<EOF

=== Everything up ===

  Grafana      : http://localhost:3000          (BPT System Overview auto-loaded)
  Prometheus   : http://localhost:9090          (targets at /targets)
  Dashboard WS : ws://localhost:8080            (bridge — the React frontend connects to this)

To bring up the frontend in a separate terminal:

  cd $ROOT/dashboard/frontend
  VITE_WS_URL=ws://localhost:8080 npm run dev

Logs:
  heimdall : tail -f $ROOT/heimdall/logs/heimdall.log
  huginn   : tail -f $ROOT/huginn/logs/huginn.log
  muninn   : tail -f $ROOT/muninn/logs/muninn.log
  fenrir   : tail -f $ROOT/fenrir/logs/fenrir.log
  bridge   : tail -f $ROOT/dashboard/bridge/logs/bridge.stdout

To stop: $ROOT/scripts/down.sh
EOF

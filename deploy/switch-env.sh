#!/bin/bash
# switch-env.sh — Switch the active BPT environment.
#
# Usage:
#   ./deploy/switch-env.sh qa-okx            # OKX demo trading
#   ./deploy/switch-env.sh qa-hyperliquid    # HL testnet
#   ./deploy/switch-env.sh live-hyperliquid  # HL mainnet (REAL MONEY)
#
# This symlinks deploy/env/active.env → deploy/env/<name>.env.
# Then reload systemd and restart the stack.

set -euo pipefail

DEPLOY_DIR="$(cd "$(dirname "$0")" && pwd)"
ENV_DIR="$DEPLOY_DIR/env"

ENV_NAME="${1:-}"
if [ -z "$ENV_NAME" ]; then
    echo "Usage: $0 <env-name>"
    echo "Available:"
    ls "$ENV_DIR"/*.env 2>/dev/null | xargs -I{} basename {} .env | grep -v active
    exit 1
fi

ENV_FILE="$ENV_DIR/$ENV_NAME.env"
if [ ! -f "$ENV_FILE" ]; then
    echo "ERROR: $ENV_FILE does not exist"
    exit 1
fi

# Safety check for live environments
if [[ "$ENV_NAME" == live-* ]]; then
    echo
    echo "WARNING: You are switching to a LIVE environment ($ENV_NAME)."
    echo "This will trade with REAL MONEY on the next stack start."
    echo
    read -r -p "Type 'CONFIRM' to continue: " reply
    if [ "$reply" != "CONFIRM" ]; then
        echo "Aborted."
        exit 1
    fi
fi

ln -sf "$ENV_FILE" "$ENV_DIR/active.env"
echo "Active environment: $ENV_NAME"
echo "  $(readlink -f "$ENV_DIR/active.env")"
echo
echo "Reload and restart:"
echo "  systemctl --user daemon-reload"
echo "  systemctl --user restart bpt-stack.target"

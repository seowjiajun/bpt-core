#!/bin/bash
# switch-env.sh — Switch the active BPT environment.
#
# Three-tier environment model (BPT_ENV in the env file declares which):
#   dev-*   Local developer laptop. BPT_ENV=dev; secrets read from
#           ~/.bpt-secrets/ (plaintext dev fallback). Testnet endpoints.
#   qa-*    QA host. BPT_ENV=qa; secrets delivered via systemd-creds.
#           Testnet endpoints. Intentionally mirrors prod — the secrets
#           loader refuses the dev fallback in qa so a misconfigured unit
#           fails loudly here instead of in prod.
#   prod-*  Production host. BPT_ENV=prod; systemd-creds from Vault/HSM.
#           MAINNET / REAL MONEY.
#
# qa-*.env files are intentionally absent until a QA host is provisioned.
# Don't repurpose qa- for local runs; use dev- instead.
#
# Usage:
#   ./deploy/switch-env.sh dev-okx            # local WSL, OKX demo
#   ./deploy/switch-env.sh dev-hyperliquid    # local WSL, HL testnet
#   ./deploy/switch-env.sh prod-hyperliquid   # HL mainnet (REAL MONEY)
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

# Safety check for prod environments
if [[ "$ENV_NAME" == prod-* ]]; then
    echo
    echo "WARNING: You are switching to a PROD environment ($ENV_NAME)."
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

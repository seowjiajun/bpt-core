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

# Coherence check: every BPT_*_CONFIG picked by the env file should reference
# the same deployment profile. A mismatch means the stack would boot with
# services on different exchanges or environments — the exact failure mode
# the profile registry was introduced to prevent.
REPO_ROOT="$(cd "$DEPLOY_DIR/.." && pwd)"

# Map env var → directory each service's WorkingDirectory uses to resolve its
# BPT_*_CONFIG path. Bridge runs from the repo root (config path is already
# repo-relative); every other service runs from its own bpt-<svc>/ dir.
declare -A SERVICE_DIR=(
    [BPT_REFDATA_CONFIG]=bpt-refdata
    [BPT_MD_GATEWAY_CONFIG]=bpt-md-gateway
    [BPT_ORDER_GATEWAY_CONFIG]=bpt-order-gateway
    [BPT_STRATEGY_CONFIG]=bpt-strategy
    [BPT_ANALYTICS_CONFIG]=bpt-analytics
    [BPT_BOOK_CONFIG]=bpt-book
    [BPT_PRICER_CONFIG]=bpt-pricer
    [BPT_BRIDGE_CONFIG]=.
)

declare -A SEEN_PROFILES
while IFS='=' read -r key val; do
    case "$key" in
        BPT_*_CONFIG) ;;
        *) continue ;;
    esac
    [ -n "$val" ] || continue
    val="${val%%#*}"; val="${val//\"/}"; val="${val// /}"
    [ -n "$val" ] || continue
    svc_dir="${SERVICE_DIR[$key]:-.}"
    toml_path="$REPO_ROOT/$svc_dir/$val"
    [ -f "$toml_path" ] || { echo "WARN: $key points at $svc_dir/$val (not found, skipping coherence check)" >&2; continue; }
    prof_line="$(grep -E '^profile_config[[:space:]]*=' "$toml_path" | head -1 || true)"
    if [ -z "$prof_line" ]; then
        # Bridge intentionally has no profile_config in TOML (uses --profile CLI).
        case "$key" in BPT_BRIDGE_CONFIG) continue ;; esac
        echo "WARN: $svc_dir/$val has no profile_config line (skipping coherence check)" >&2
        continue
    fi
    prof_path="$(echo "$prof_line" | sed -E 's/^profile_config[[:space:]]*=[[:space:]]*"([^"]+)".*/\1/')"
    SEEN_PROFILES["$(basename "$prof_path")"]+="${key},"
done < "$ENV_FILE"

# Bridge's --profile path participates too — fold it in by basename.
bridge_prof="$(grep -E '^BPT_BRIDGE_PROFILE=' "$ENV_FILE" | head -1 | cut -d= -f2-)"
bridge_prof="${bridge_prof%%#*}"; bridge_prof="${bridge_prof//\"/}"; bridge_prof="${bridge_prof// /}"
if [ -n "$bridge_prof" ]; then
    SEEN_PROFILES["$(basename "$bridge_prof")"]+="BPT_BRIDGE_PROFILE,"
fi

if [ "${#SEEN_PROFILES[@]}" -gt 1 ]; then
    echo "ERROR: $ENV_FILE points at configs that disagree on profile_config:" >&2
    for bn in "${!SEEN_PROFILES[@]}"; do
        echo "  $bn — used by: ${SEEN_PROFILES[$bn]%,}" >&2
    done
    echo "Refusing to activate — fix the env file so every service uses the same profile." >&2
    exit 1
fi
if [ "${#SEEN_PROFILES[@]}" -eq 1 ]; then
    for bn in "${!SEEN_PROFILES[@]}"; do
        echo "Coherence check: all services agree on profile=$bn"
    done
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

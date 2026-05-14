#!/bin/bash
# bootstrap.sh — run on the trading host once cloud-init has finished.
#
# Idempotent: safe to re-run after stop/start cycles or after pulling new
# code. Builds the trading-stack binaries with bazel and installs the
# systemd user units. Does NOT start the stack — operator decides when
# via systemctl --user start bpt-stack.target.
#
# Usage (from operator workstation, after `terraform apply`):
#   scp infra/terraform/trading-host/bootstrap.sh ubuntu@$EIP:/tmp/
#   ssh ubuntu@$EIP 'bash /tmp/bootstrap.sh'

set -euxo pipefail

# ── Wait for cloud-init to complete ──────────────────────────────────────
echo "Waiting for cloud-init to complete..."
for _ in $(seq 1 60); do
    [[ -f /var/log/bpt-cloud-init.done ]] && break
    sleep 5
done
[[ -f /var/log/bpt-cloud-init.done ]] || { echo "ERROR: cloud-init did not complete" >&2; exit 1; }

# ── Install build deps that cloud-init left out ──────────────────────────
sudo apt-get update
sudo apt-get install -y \
    g++ \
    cmake \
    pkg-config \
    libssl-dev \
    libtool \
    autoconf \
    automake \
    python3-pip \
    python3-venv \
    libsystemd-dev \
    uuid-dev

# ── Install bazelisk (pins to .bazelversion in the repo) ─────────────────
if ! command -v bazel >/dev/null 2>&1; then
    sudo curl -fsSL -o /usr/local/bin/bazel \
        https://github.com/bazelbuild/bazelisk/releases/latest/download/bazelisk-linux-amd64
    sudo chmod +x /usr/local/bin/bazel
fi

# ── Clone or update the repo on the data volume ──────────────────────────
cd /opt/bpt
if [[ ! -d code/bpt-core/.git ]]; then
    git clone https://github.com/bishanparktrading/bpt-core.git code/bpt-core
fi
cd /opt/bpt/code/bpt-core
git fetch origin
git checkout main
git pull --ff-only

# ── Build the trading-stack binaries ─────────────────────────────────────
# First run: ~15-20 min (cold bazel cache, downloads deps).
# Subsequent: ~30 s to a few min depending on what changed.
bazel build \
    //bpt-strategy:bpt-strategy \
    //bpt-md-gateway:bpt-md-gateway \
    //bpt-order-gateway:bpt-order-gateway \
    //bpt-refdata:bpt-refdata \
    //bpt-analytics:bpt-analytics \
    //bpt-pms:bpt-pms \
    //bpt-bridge:bpt-bridge

# ── Stage a default .env (HL testnet, funding-arb path) ──────────────────
# Operator can switch envs later via ./deploy/switch-env.sh.
ENV_DIR=/opt/bpt/code/bpt-core/deploy/env
if [[ ! -f $ENV_DIR/dev-hyperliquid-funding-arb.env ]]; then
    cp $ENV_DIR/dev-hyperliquid-funding-arb.env.example \
       $ENV_DIR/dev-hyperliquid-funding-arb.env
fi

# Activate the FA env by default. Operator overrides with
# ./deploy/switch-env.sh <other-env> as needed.
ln -sf $ENV_DIR/dev-hyperliquid-funding-arb.env $ENV_DIR/active.env

# ── Install systemd user units ───────────────────────────────────────────
# Pulls from the repo's deploy/ tree.
sudo loginctl enable-linger ubuntu
mkdir -p ~/.config/systemd/user
cp /opt/bpt/code/bpt-core/deploy/systemd/user/*.{service,target,timer} \
   ~/.config/systemd/user/ 2>/dev/null || \
   echo "NOTE: systemd unit dir layout differs — run deploy/generate-units.sh manually"

systemctl --user daemon-reload

# ── Tailscale enrollment reminder ────────────────────────────────────────
echo ""
echo "─────────────────────────────────────────────────────────────────────"
echo " Bootstrap complete."
echo ""
echo " NEXT STEPS:"
echo "   1. Enroll this host in Tailscale (interactive — opens a browser link):"
echo "        sudo tailscale up"
echo ""
echo "   2. Drop in HL credentials at ~/.bpt-secrets/bpt-testnet-HYPERLIQUID:"
echo "        HYPERLIQUID_PRIVATE_KEY=0x..."
echo "        HYPERLIQUID_WALLET_ADDRESS=0x..."
echo "      chmod 0600 ~/.bpt-secrets/bpt-testnet-HYPERLIQUID"
echo ""
echo "   3. Start the stack:"
echo "        systemctl --user start bpt-stack.target"
echo ""
echo "   4. Watch logs:"
echo "        journalctl --user -fu bpt-strategy"
echo "─────────────────────────────────────────────────────────────────────"

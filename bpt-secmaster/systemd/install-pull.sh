#!/bin/bash
# One-time bootstrap of the bpt-secmaster snapshot puller on the trading
# host. Idempotent — safe to re-run for upgrades.
#
# Prerequisites:
#   1. aws-cli installed + configured with creds that have s3:GetObject
#      on s3://bpt-tape-archive/secmaster/*
#   2. /opt/bpt/data/ directory exists (the trading-host deploy creates this)
#   3. systemd
#
# Run on the trading host as root (sudo).

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Must run as root (sudo)" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── 1. Ensure target directory exists ─────────────────────────────────
mkdir -p /opt/bpt/data

# ── 2. Install pull script ────────────────────────────────────────────
echo "==> Installing /usr/local/bin/bpt-secmaster-pull"
install -m 0755 "$SCRIPT_DIR/bpt-secmaster-pull.sh" /usr/local/bin/bpt-secmaster-pull

# ── 3. Install systemd unit + timer ───────────────────────────────────
echo "==> Installing /etc/systemd/system/bpt-secmaster-pull.{service,timer}"
install -m 0644 "$SCRIPT_DIR/bpt-secmaster-pull.service" /etc/systemd/system/bpt-secmaster-pull.service
install -m 0644 "$SCRIPT_DIR/bpt-secmaster-pull.timer"   /etc/systemd/system/bpt-secmaster-pull.timer

# ── 4. Enable + start ─────────────────────────────────────────────────
echo "==> Reloading systemd, enabling + starting timer"
systemctl daemon-reload
systemctl enable bpt-secmaster-pull.timer
systemctl start bpt-secmaster-pull.timer

# ── 5. Trigger an immediate pull so the file is present right away ──
echo "==> Triggering immediate pull"
if systemctl start bpt-secmaster-pull.service; then
    echo ""
    echo "✓ pull complete. Latest snapshot at /opt/bpt/data/instrument_mapping.json"
    ls -la /opt/bpt/data/instrument_mapping.json 2>/dev/null || true
    echo ""
    echo "Next pulls will happen automatically (hourly + at boot)."
    echo "Logs:    journalctl -u bpt-secmaster-pull -f"
    echo "Status:  systemctl list-timers bpt-secmaster-pull"
else
    echo "✗ pull failed. Check: journalctl -u bpt-secmaster-pull --no-pager -n 20" >&2
    exit 1
fi

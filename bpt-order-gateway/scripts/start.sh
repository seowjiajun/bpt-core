#!/bin/bash
# start.sh — systemd wrapper. Routes through systemctl so there's a single
# source of truth for service state. Prevents dual-start divergence with
# systemd-managed instances (the historical footgun).
#
# Config comes from $BPT_ORDER_GATEWAY_CONFIG in deploy/env/active.env, which the
# systemd unit reads via EnvironmentFile. To change config, run:
#   ./deploy/switch-env.sh <env-name>
# before starting.

set -euo pipefail

UNIT="bpt-order-gateway.service"

if [[ $# -gt 0 ]]; then
    echo "NOTE: $0 is a systemd wrapper; config arg ignored." >&2
    echo "      Config comes from deploy/env/active.env (\$BPT_ORDER_GATEWAY_CONFIG)." >&2
    echo "      Run deploy/switch-env.sh <env> to change it." >&2
fi

systemctl --user start "$UNIT"
sleep 1
state=$(systemctl --user is-active "$UNIT" || true)
echo "$UNIT: $state"
[[ "$state" == "active" ]]

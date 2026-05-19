#!/bin/bash
# dev_start.sh — systemd wrapper. Routes through systemctl so service state
# stays in sync with what systemd thinks is running.

set -euo pipefail

UNIT="bpt-transport.service"

systemctl --user start "$UNIT"
sleep 1
state=$(systemctl --user is-active "$UNIT" || true)
echo "$UNIT: $state"
[[ "$state" == "active" ]]

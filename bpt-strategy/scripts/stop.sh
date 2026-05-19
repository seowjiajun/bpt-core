#!/bin/bash
# stop.sh — systemd wrapper. Stops via systemctl so state matches what
# systemd thinks is running.

set -euo pipefail

UNIT="bpt-strategy.service"

systemctl --user stop "$UNIT"
echo "$UNIT stopped"

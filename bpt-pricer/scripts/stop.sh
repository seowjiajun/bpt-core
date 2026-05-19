#!/bin/bash
# stop.sh — systemd wrapper. Stops via systemctl so state matches what
# systemd thinks is running.

set -euo pipefail

UNIT="bpt-pricer.service"

systemctl --user stop "$UNIT"
echo "$UNIT stopped"

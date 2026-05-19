#!/bin/bash
# dev_stop.sh — systemd wrapper. Stops via systemctl.

set -euo pipefail

UNIT="bpt-transport.service"

systemctl --user stop "$UNIT"
echo "$UNIT stopped"

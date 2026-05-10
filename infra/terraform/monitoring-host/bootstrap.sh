#!/bin/bash
# bootstrap.sh — finish setting up the monitoring host after `terraform
# apply` has created the EC2 + EBS + EIP + SG.
#
# Cloud-init handled: docker-engine, tailscale daemon (not yet upped),
# data-disk format + mount at /opt/bpt-monitoring/data.
#
# This script (run by operator over SSH from their laptop) handles:
#   1. tailscale up — operator authenticates via browser link
#   2. install monitoring-stack files (operator SCP'd them to /tmp first)
#   3. template prometheus.yml with the tape host's private IP
#   4. docker compose up -d
#   5. quick smoke test that all targets are scraping
#
# Usage:
#   # ON OPERATOR LAPTOP, before SSHing:
#   scp -i ~/.ssh/<key> -r monitoring/ infra/terraform/monitoring-host/bootstrap.sh \
#       infra/terraform/monitoring-host/prometheus.yml.tmpl \
#       ubuntu@<eip>:/tmp/
#
#   # SSH to monitor host:
#   ssh -i ~/.ssh/<key> ubuntu@<eip>
#   sudo bash /tmp/bootstrap.sh <TAPE_HOST_PRIVATE_IP>
#
# TAPE_HOST_PRIVATE_IP comes from `terraform output tape_host_private_ip`.

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <TAPE_HOST_PRIVATE_IP>"
    echo "  get the IP from: terraform output tape_host_private_ip"
    exit 2
fi

TAPE_HOST_PRIVATE_IP="$1"
MONITORING_DIR="/opt/bpt-monitoring/stack"
DATA_DIR="/opt/bpt-monitoring/data"

log() { echo "[bootstrap $(date -u +%H:%M:%SZ)] $*"; }

# 1. Tailscale
# ------------------------------------------------------------------
# `tailscale up` will print a https://login.tailscale.com/a/... URL.
# Click it on your laptop's browser (it auto-authenticates with the
# Google identity you've already logged in with — no auth key handling
# required). Once approved in the admin console, tailscaled stays up
# across reboots. Hostname registers as the EC2 hostname by default;
# rename in the Tailscale admin console if you want monitor-tokyo-01.
log "starting tailscale (you'll get a URL — click it from your laptop)"
sudo tailscale up --ssh --hostname=bpt-monitor-tokyo-01

log "tailscale status:"
tailscale status || true

# 2. Lay out the monitoring stack on the data disk
# ------------------------------------------------------------------
# The monitoring/ dir was SCP'd to /tmp before this script ran.
# Move it to /opt/bpt-monitoring/stack so the data-disk EBS holds
# both the source and the prometheus tsdb (single volume to snapshot).
log "installing monitoring stack at $MONITORING_DIR"
sudo mkdir -p "$MONITORING_DIR"
sudo cp -r /tmp/monitoring/* "$MONITORING_DIR/"
sudo chown -R ubuntu:ubuntu "$MONITORING_DIR"

# 3. Render prometheus.yml from the in-VPC template
# ------------------------------------------------------------------
log "rendering prometheus.yml with TAPE_HOST_PRIVATE_IP=$TAPE_HOST_PRIVATE_IP"
sudo cp /tmp/prometheus.yml.tmpl "$MONITORING_DIR/prometheus.yml"
sudo sed -i "s|{{TAPE_HOST_PRIVATE_IP}}|$TAPE_HOST_PRIVATE_IP|g" \
    "$MONITORING_DIR/prometheus.yml"

# 4. Point Prometheus tsdb at the data disk
# ------------------------------------------------------------------
# The default monitoring/docker-compose.yml uses a docker-managed named
# volume. We want the tsdb on the data EBS so it survives root-volume
# rebuilds. Patch docker-compose.yml to bind /opt/bpt-monitoring/data
# instead.
log "redirecting prometheus tsdb to $DATA_DIR/prometheus"
sudo mkdir -p "$DATA_DIR/prometheus" "$DATA_DIR/grafana"
sudo chown -R 65534:65534 "$DATA_DIR/prometheus"   # nobody:nogroup, prometheus container runs as
sudo chown -R 472:472     "$DATA_DIR/grafana"      # grafana container's user/group

# Best-effort patch of compose's named volume → bind mount. If the
# operator wants something different, hand-edit after this script runs.
if grep -q "prometheus-data:" "$MONITORING_DIR/docker-compose.yml"; then
    log "(patch) named volume prometheus-data → bind mount $DATA_DIR/prometheus"
    sudo sed -i \
        "s|prometheus-data:/prometheus|$DATA_DIR/prometheus:/prometheus|" \
        "$MONITORING_DIR/docker-compose.yml"
fi
if grep -q "grafana-data:" "$MONITORING_DIR/docker-compose.yml"; then
    log "(patch) named volume grafana-data → bind mount $DATA_DIR/grafana"
    sudo sed -i \
        "s|grafana-data:/var/lib/grafana|$DATA_DIR/grafana:/var/lib/grafana|" \
        "$MONITORING_DIR/docker-compose.yml"
fi

# 5. Bring up the stack
# ------------------------------------------------------------------
log "docker compose up -d"
cd "$MONITORING_DIR"
sudo docker compose up -d
sleep 5

log "running containers:"
sudo docker compose ps

# 6. Smoke test
# ------------------------------------------------------------------
log "Prometheus targets:"
curl -s http://localhost:9090/api/v1/targets 2>/dev/null \
    | python3 -c "
import sys, json
try:
    d = json.load(sys.stdin)
    for t in d['data']['activeTargets']:
        print(f\"  {t['labels'].get('job','?'):<20} {t['health']:<7} {t['lastError'] or 'ok'}\")
except Exception as e:
    print(f'  (could not parse: {e})')
"

cat <<EOF

==================================================================
bootstrap complete

Next steps (do these from your laptop, on your tailnet):

1.  Confirm Grafana is reachable via tailnet:
       http://bpt-monitor-tokyo-01:3000
       (login: admin / admin — change on first login)

2.  Verify the bpt-tape dashboard has live data:
       http://bpt-monitor-tokyo-01:3000/d/bpt-tape/bpt-tape

3.  Once Tailscale access is verified working, lock down public SSH:
       In the AWS console (or via terraform var override), drop
       operator_ssh_cidr to 127.0.0.1/32 (effectively closed). You'll
       still be able to SSH via Tailscale's MagicDNS hostname.

4.  Tear down the laptop's local docker-compose monitoring stack and
    the SSH tunnels — both are now redundant:
       cd ~/code/bpt-core/monitoring && docker compose down
       pkill -f "ssh.*-L 0.0.0.0:911[12]"
       pkill -f "ssh.*-L 0.0.0.0:9120"

==================================================================
EOF

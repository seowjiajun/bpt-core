#!/bin/bash
# generate-units.sh — Generate systemd user units for the BPT trading stack
# (production/live variant). Writes units to ~/.config/systemd/user/.
#
# Binaries are taken from bazel-bin/ (Bazel-built). The one exception used to
# be bpt-refdata (blocked on aws-sdk-cpp); now that refdata is AWS-free, the
# whole stack runs off bazel-bin/ and CMake is no longer referenced here.
#
# The config-sync timer is also emitted — it git-pulls the repo daily so that
# mapping/fee/config PRs merged to main propagate to the running services on
# their next refresh tick. Code changes still ship via release tarballs.
set -euo pipefail

BPT_ROOT="/home/jseow/code/bpt-core"
UNIT_DIR="$HOME/.config/systemd/user"
ENV_FILE="$BPT_ROOT/deploy/env/active.env"

mkdir -p "$UNIT_DIR"

# ── Binary path resolution (all Bazel-built) ────────────────────────────────
declare -A BIN
BIN[bpt-refdata]="$BPT_ROOT/bazel-bin/bpt-refdata/bpt-refdata"
BIN[bpt-md-gateway]="$BPT_ROOT/bazel-bin/bpt-md-gateway/bpt-md-gateway"
BIN[bpt-order-gateway]="$BPT_ROOT/bazel-bin/bpt-order-gateway/bpt-order-gateway"
BIN[bpt-strategy]="$BPT_ROOT/bazel-bin/bpt-strategy/bpt-strategy"
BIN[bpt-analytics]="$BPT_ROOT/bazel-bin/bpt-analytics/bpt-analytics"
BIN[bpt-bridge]="$BPT_ROOT/bazel-bin/dashboard/bridge/bridge"

# ── bpt-transport (Aeron media driver, Gradle-built Java) ───────────────────
cat > "$UNIT_DIR/bpt-transport.service" <<EOF
[Unit]
Description=BPT Transport (Aeron MediaDriver)
PartOf=bpt-stack.target

[Service]
Type=simple
WorkingDirectory=$BPT_ROOT/transport
ExecStart=/usr/bin/java --add-opens java.base/sun.nio.ch=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED -jar $BPT_ROOT/transport/aeron/build/libs/bifrost-fabric-1.0.0-all.jar --config $BPT_ROOT/transport/aeron/config/config.yaml
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-{refdata,md-gateway,order-gateway} ──────────────────────────────────
# refdata and order-gateway need exchange credentials. systemd delivers them
# via LoadCredentialEncrypted= — decrypted files land in $CREDENTIALS_DIRECTORY
# keyed by secret_name. Configs reference bpt/testnet/OKX → bpt-testnet-OKX.
CRED_DIR="$HOME/.config/systemd/creds"
for svc in bpt-refdata bpt-md-gateway bpt-order-gateway; do
    env_key=$(echo "$svc" | sed 's/^bpt-//' | tr '[:lower:]-' '[:upper:]_')
    cfg_var="BPT_${env_key}_CONFIG"

    after="bpt-transport.service"
    [ "$svc" = "bpt-md-gateway" ] && after="bpt-refdata.service"
    [ "$svc" = "bpt-order-gateway" ] && after="bpt-refdata.service"

    creds_lines=""
    if [ "$svc" = "bpt-refdata" ] || [ "$svc" = "bpt-order-gateway" ]; then
        for cred in bpt-testnet-OKX; do
            if [ -f "$CRED_DIR/$cred.cred" ]; then
                creds_lines+="LoadCredentialEncrypted=$cred:$CRED_DIR/$cred.cred
"
            fi
        done
    fi

    cat > "$UNIT_DIR/$svc.service" <<EOF
[Unit]
Description=BPT ${svc^} Service
After=$after
Requires=bpt-transport.service
PartOf=bpt-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/$svc
ExecStart=${BIN[$svc]} --config \${$cfg_var}
${creds_lines}Restart=on-failure
RestartSec=3
TimeoutStopSec=15

[Install]
WantedBy=bpt-stack.target
EOF
done

# ── bpt-strategy ─────────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-strategy.service" <<EOF
[Unit]
Description=BPT Strategy Engine
After=bpt-md-gateway.service bpt-order-gateway.service
Requires=bpt-transport.service
PartOf=bpt-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-strategy
ExecStart=${BIN[bpt-strategy]} --config \${BPT_STRATEGY_CONFIG}
Restart=on-failure
RestartSec=5
TimeoutStopSec=30

[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-analytics ────────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-analytics.service" <<EOF
[Unit]
Description=BPT Analytics (markouts, toxicity, fill rate)
After=bpt-strategy.service
Requires=bpt-transport.service
PartOf=bpt-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-analytics
ExecStart=${BIN[bpt-analytics]} --config \${BPT_ANALYTICS_CONFIG}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-bridge (dashboard WebSocket forwarder) ──────────────────────────────
cat > "$UNIT_DIR/bpt-bridge.service" <<EOF
[Unit]
Description=BPT Dashboard Bridge
After=bpt-strategy.service
Requires=bpt-transport.service
PartOf=bpt-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT
ExecStart=${BIN[bpt-bridge]} --config \${BPT_BRIDGE_CONFIG} --mode \${BPT_BRIDGE_MODE} --strategy-name \${BPT_BRIDGE_STRATEGY} --symbol \${BPT_BRIDGE_SYMBOL} --exchange \${BPT_BRIDGE_EXCHANGE} --instrument-type \${BPT_BRIDGE_INST_TYPE}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-frontend (dashboard UI, Vite dev server) ────────────────────────────
cat > "$UNIT_DIR/bpt-frontend.service" <<EOF
[Unit]
Description=BPT Dashboard Frontend
After=bpt-bridge.service
PartOf=bpt-stack.target

[Service]
Type=simple
WorkingDirectory=$BPT_ROOT/dashboard/frontend
Environment=VITE_WS_URL=ws://localhost:8080
Environment=PATH=/home/jseow/.nvm/versions/node/v20.20.1/bin:/usr/bin
ExecStart=/home/jseow/.nvm/versions/node/v20.20.1/bin/node $BPT_ROOT/dashboard/frontend/node_modules/.bin/vite
Restart=on-failure
RestartSec=3
TimeoutStopSec=5

[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-config-sync.{service,timer} ─────────────────────────────────────────
# Pulls config changes (instrument mapping, fees, service TOMLs) from origin
# daily. Services pick up the new files on their next internal refresh tick;
# nothing restarts.
cat > "$UNIT_DIR/bpt-config-sync.service" <<EOF
[Unit]
Description=BPT Config Sync (git pull for config-only updates)

[Service]
Type=oneshot
ExecStart=$BPT_ROOT/deploy/sync-config.sh
Environment=BPT_ROOT=$BPT_ROOT
EOF

cat > "$UNIT_DIR/bpt-config-sync.timer" <<EOF
[Unit]
Description=Daily BPT config sync (runs at 06:00 local, 2h after the GHA cron)

[Timer]
OnCalendar=*-*-* 06:00:00
Persistent=true

[Install]
WantedBy=timers.target
EOF

# ── bpt-prometheus / bpt-alertmanager / bpt-heartbeat ───────────────────────
# Monitoring stack — see deploy/monitoring/README.md for the full story.
#
# Prometheus scrapes every service's /metrics endpoint and evaluates the alert
# rules under deploy/monitoring/prometheus/rules/.  Alertmanager receives
# firing alerts and posts to Discord.  bpt-heartbeat.timer pings Healthchecks.io
# every 5 min as a dead-man's-switch for the host-down case.

cat > "$UNIT_DIR/bpt-prometheus.service" <<EOF
[Unit]
Description=BPT Prometheus (scraper + alert rule evaluator)
PartOf=bpt-stack.target
After=multi-user.target

[Service]
Type=simple
ExecStart=/usr/bin/prometheus \\
  --config.file=$BPT_ROOT/deploy/monitoring/prometheus/prometheus.yml \\
  --storage.tsdb.path=%h/.local/share/prometheus/data \\
  --storage.tsdb.retention.time=14d \\
  --web.listen-address=127.0.0.1:9090
Restart=on-failure
RestartSec=3

[Install]
WantedBy=bpt-stack.target
EOF

# Alertmanager: loads the Discord webhook URL via LoadCredentialEncrypted
# (systemd-creds).  The creds file should already exist at
# ~/.config/systemd/creds/bpt-discord-webhook.cred — see monitoring README.
#
# Binary path: Debian/Ubuntu ships the Alertmanager binary as
# /usr/bin/prometheus-alertmanager (namespaced to avoid conflicts with
# other "alertmanager"-named tools).  If you installed via the upstream
# tarball, it's likely /usr/local/bin/alertmanager instead — edit
# ExecStart to match.
cat > "$UNIT_DIR/bpt-alertmanager.service" <<EOF
[Unit]
Description=BPT Alertmanager (Discord webhook dispatch)
PartOf=bpt-stack.target
After=bpt-prometheus.service

[Service]
Type=simple
# Discord webhook URL — decrypted into \$CREDENTIALS_DIRECTORY at start.
# The alertmanager.yml references webhook_url_file pointing to this path.
LoadCredentialEncrypted=bpt-discord-webhook:$CRED_DIR/bpt-discord-webhook.cred
ExecStart=/usr/bin/prometheus-alertmanager \\
  --config.file=$BPT_ROOT/deploy/monitoring/alertmanager/alertmanager.yml \\
  --storage.path=%h/.local/share/alertmanager \\
  --web.listen-address=127.0.0.1:9093
Restart=on-failure
RestartSec=3

[Install]
WantedBy=bpt-stack.target
EOF

# Heartbeat — dead-man's-switch so Healthchecks.io emails you when the
# host itself is dead (Alertmanager can't alert on its own process death).
# HC_URL lives in /etc/bpt/healthchecks.env, keyed per-host.
cat > "$UNIT_DIR/bpt-heartbeat.service" <<EOF
[Unit]
Description=BPT Healthchecks.io dead-man's-switch ping

[Service]
Type=oneshot
EnvironmentFile=/etc/bpt/healthchecks.env
ExecStart=/usr/bin/curl -fsS -m 10 --retry 3 \${HC_URL}
EOF

cat > "$UNIT_DIR/bpt-heartbeat.timer" <<EOF
[Unit]
Description=BPT heartbeat ping every 5 min
PartOf=bpt-stack.target

[Timer]
OnBootSec=30s
OnUnitActiveSec=5min
AccuracySec=30s

[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-stack.target ─────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-stack.target" <<EOF
[Unit]
Description=BPT Trading Stack
Wants=bpt-transport.service bpt-refdata.service bpt-md-gateway.service bpt-order-gateway.service bpt-strategy.service bpt-analytics.service bpt-bridge.service bpt-frontend.service bpt-prometheus.service bpt-alertmanager.service bpt-heartbeat.timer

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload

echo "Generated $(ls "$UNIT_DIR"/bpt-*.service "$UNIT_DIR"/bpt-*.timer "$UNIT_DIR"/bpt-*.target 2>/dev/null | wc -l) units in $UNIT_DIR"
echo "Active env: $(readlink -f "$ENV_FILE" 2>/dev/null || echo "$ENV_FILE")"
echo
echo "Enable + start:"
echo "  systemctl --user enable --now bpt-config-sync.timer   # daily config pull"
echo "  systemctl --user enable --now bpt-heartbeat.timer     # dead-man's-switch to Healthchecks.io"
echo "  systemctl --user start bpt-stack.target               # bring the stack up"
echo
echo "Monitoring setup (see deploy/monitoring/README.md):"
echo "  sudo apt install prometheus prometheus-alertmanager"
echo "  sudo systemctl disable --now prometheus prometheus-alertmanager  # we run as user units"
echo "  echo -n 'https://discord.com/api/webhooks/...' | \\"
echo "    systemd-creds encrypt - $CRED_DIR/bpt-discord-webhook.cred"
echo "  echo 'HC_URL=https://hc-ping.com/YOUR-UUID' | sudo tee /etc/bpt/healthchecks.env"
echo
echo "Inspect:"
echo "  systemctl --user status 'bpt-*'"
echo "  systemctl --user list-timers bpt-config-sync.timer"
echo "  journalctl --user -u bpt-config-sync -f   # tail sync logs"

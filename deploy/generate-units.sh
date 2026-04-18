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
ExecStart=${BIN[$svc]} $([ "$svc" = "bpt-refdata" ] && echo "--config") \${$cfg_var}
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
ExecStart=${BIN[bpt-strategy]} \${BPT_STRATEGY_CONFIG}
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
ExecStart=${BIN[bpt-analytics]} \${BPT_ANALYTICS_CONFIG}
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

# ── bpt-stack.target ─────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-stack.target" <<EOF
[Unit]
Description=BPT Trading Stack
Wants=bpt-transport.service bpt-refdata.service bpt-md-gateway.service bpt-order-gateway.service bpt-strategy.service bpt-analytics.service bpt-bridge.service bpt-frontend.service

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload

echo "Generated $(ls "$UNIT_DIR"/bpt-*.service "$UNIT_DIR"/bpt-*.timer "$UNIT_DIR"/bpt-*.target 2>/dev/null | wc -l) units in $UNIT_DIR"
echo "Active env: $(readlink -f "$ENV_FILE" 2>/dev/null || echo "$ENV_FILE")"
echo
echo "Enable + start:"
echo "  systemctl --user enable --now bpt-config-sync.timer   # daily config pull"
echo "  systemctl --user start bpt-stack.target               # bring the stack up"
echo
echo "Inspect:"
echo "  systemctl --user status 'bpt-*'"
echo "  systemctl --user list-timers bpt-config-sync.timer"
echo "  journalctl --user -u bpt-config-sync -f   # tail sync logs"

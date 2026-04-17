#!/bin/bash
# generate-units.sh — Generates systemd user units for the BPT trading stack.
set -euo pipefail

BPT_ROOT="/home/jseow/code/bpt-core"
UNIT_DIR="$HOME/.config/systemd/user"
ENV_FILE="$BPT_ROOT/deploy/env/active.env"

mkdir -p "$UNIT_DIR"

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

for svc in bpt-refdata bpt-md-gateway bpt-order-gateway; do
    # Strip bpt- prefix for env var lookup: bpt-md-gateway → MD_GATEWAY
    env_key=$(echo "$svc" | sed 's/^bpt-//' | tr '[:lower:]-' '[:upper:]_')
    cfg_var="BPT_${env_key}_CONFIG"

    after="bpt-transport.service"
    [ "$svc" = "bpt-md-gateway" ] && after="bpt-refdata.service"
    [ "$svc" = "bpt-order-gateway" ] && after="bpt-refdata.service"

    # Services without bpt- prefix get it added for the unit name
    unit_name="$svc"
    [[ "$svc" != bpt-* ]] && unit_name="bpt-$svc"

    cat > "$UNIT_DIR/$unit_name.service" <<EOF
[Unit]
Description=BPT ${svc^} Service
After=$after
Requires=bpt-transport.service
PartOf=bpt-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/$svc
ExecStart=$BPT_ROOT/build/$svc/src/$svc $([ "$svc" = "bpt-refdata" ] && echo "--config") \${$cfg_var}
Restart=on-failure
RestartSec=3
TimeoutStopSec=15

[Install]
WantedBy=bpt-stack.target
EOF
done

cat > "$UNIT_DIR/bpt-strategy.service" <<EOF
[Unit]
Description=BPT Strategy Strategy Engine
After=bpt-md-gateway.service bpt-order-gateway.service
Requires=bpt-transport.service
PartOf=bpt-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-strategy
ExecStart=$BPT_ROOT/build/bpt-strategy/src/bpt-strategy \${BPT_STRATEGY_CONFIG}
Restart=on-failure
RestartSec=5
TimeoutStopSec=30

[Install]
WantedBy=bpt-stack.target
EOF

cat > "$UNIT_DIR/bpt-analytics.service" <<EOF
[Unit]
Description=BPT Analytics Toxic Flow Analyzer
After=bpt-strategy.service
Requires=bpt-transport.service
PartOf=bpt-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-analytics
ExecStart=$BPT_ROOT/build/bpt-analytics/src/bpt-analytics \${BPT_ANALYTICS_CONFIG}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=bpt-stack.target
EOF

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
ExecStart=$BPT_ROOT/build/dashboard/bridge/bridge --config \${BPT_BRIDGE_CONFIG} --mode \${BPT_BRIDGE_MODE} --strategy-name \${BPT_BRIDGE_STRATEGY} --symbol \${BPT_BRIDGE_SYMBOL} --exchange \${BPT_BRIDGE_EXCHANGE} --instrument-type \${BPT_BRIDGE_INST_TYPE}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=bpt-stack.target
EOF

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

cat > "$UNIT_DIR/bpt-stack.target" <<EOF
[Unit]
Description=BPT Trading Stack
Wants=bpt-transport.service bpt-refdata.service bpt-md-gateway.service bpt-order-gateway.service bpt-strategy.service bpt-analytics.service bpt-bridge.service bpt-frontend.service

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
echo "Generated $(ls "$UNIT_DIR"/bpt-* | wc -l) units in $UNIT_DIR"
echo "Active env: $(readlink -f "$ENV_FILE")"
echo
echo "Usage:"
echo "  systemctl --user start bpt-stack.target    # start everything"
echo "  systemctl --user stop bpt-stack.target     # stop everything"
echo "  systemctl --user status 'bpt-*'            # check all services"
echo "  journalctl --user -u bpt-strategy -f         # tail bpt-strategy logs"

#!/bin/bash
# generate-units-dev.sh — Generates systemd user units for the BPT dev stack.
#
# Differences from generate-units.sh:
#   1. Unit names prefixed bpt-dev-* (so they don't collide with the prod set)
#   2. ExecStart points at Bazel-built binaries in bazel-bin/
#      (exception: bpt-refdata still uses the CMake build — AWS SDK blocker)
#   3. Restart=no — dev should surface crashes, not hide them in restart loops
#   4. Own env file (deploy/env/dev.env) with isolated Aeron dir + metrics ports
#   5. Own stack target (bpt-dev-stack.target)
#   6. Dev transport points at config-dev.yaml so the prod stack can run at the
#      same time on the same machine.
set -euo pipefail

BPT_ROOT="/home/jseow/code/bpt-core"
UNIT_DIR="$HOME/.config/systemd/user"
ENV_FILE="$BPT_ROOT/deploy/env/dev.env"

mkdir -p "$UNIT_DIR"

# ── bpt-dev-transport (Aeron media driver, dev instance) ─────────────────────
cat > "$UNIT_DIR/bpt-dev-transport.service" <<EOF
[Unit]
Description=BPT Transport — Aeron MediaDriver (dev)
PartOf=bpt-dev-stack.target

[Service]
Type=simple
WorkingDirectory=$BPT_ROOT/transport
ExecStart=/usr/bin/java --add-opens java.base/sun.nio.ch=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED -jar $BPT_ROOT/transport/aeron/build/libs/bifrost-fabric-1.0.0-all.jar --config $BPT_ROOT/transport/aeron/config/config-dev.yaml
Restart=no
TimeoutStopSec=10

[Install]
WantedBy=bpt-dev-stack.target
EOF

# ── Binary path resolution (Bazel for most; CMake for refdata) ───────────────
declare -A BIN
BIN[bpt-refdata]="$BPT_ROOT/bazel-bin/bpt-refdata/bpt-refdata"
BIN[bpt-md-gateway]="$BPT_ROOT/bazel-bin/bpt-md-gateway/bpt-md-gateway"
BIN[bpt-order-gateway]="$BPT_ROOT/bazel-bin/bpt-order-gateway/bpt-order-gateway"
BIN[bpt-strategy]="$BPT_ROOT/bazel-bin/bpt-strategy/bpt-strategy"
BIN[bpt-analytics]="$BPT_ROOT/bazel-bin/bpt-analytics/bpt-analytics"
BIN[bpt-bridge]="$BPT_ROOT/bazel-bin/dashboard/bridge/bridge"

# ── bpt-dev-{refdata,md-gateway,order-gateway} ───────────────────────────────
for svc in bpt-refdata bpt-md-gateway bpt-order-gateway; do
    env_key=$(echo "$svc" | sed 's/^bpt-//' | tr '[:lower:]-' '[:upper:]_')
    cfg_var="BPT_${env_key}_CONFIG"

    after="bpt-dev-transport.service"
    [ "$svc" = "bpt-md-gateway" ] && after="bpt-dev-refdata.service"
    [ "$svc" = "bpt-order-gateway" ] && after="bpt-dev-refdata.service"

    cat > "$UNIT_DIR/bpt-dev-${svc#bpt-}.service" <<EOF
[Unit]
Description=BPT ${svc^} (dev)
After=$after
Requires=bpt-dev-transport.service
PartOf=bpt-dev-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/$svc
ExecStart=${BIN[$svc]} $([ "$svc" = "bpt-refdata" ] && echo "--config") \${$cfg_var}
Restart=no
TimeoutStopSec=15

[Install]
WantedBy=bpt-dev-stack.target
EOF
done

# ── bpt-dev-strategy ─────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-dev-strategy.service" <<EOF
[Unit]
Description=BPT Strategy (dev)
After=bpt-dev-md-gateway.service bpt-dev-order-gateway.service
Requires=bpt-dev-transport.service
PartOf=bpt-dev-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-strategy
ExecStart=${BIN[bpt-strategy]} \${BPT_STRATEGY_CONFIG}
Restart=no
TimeoutStopSec=30

[Install]
WantedBy=bpt-dev-stack.target
EOF

# ── bpt-dev-analytics ────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-dev-analytics.service" <<EOF
[Unit]
Description=BPT Analytics (dev)
After=bpt-dev-strategy.service
Requires=bpt-dev-transport.service
PartOf=bpt-dev-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-analytics
ExecStart=${BIN[bpt-analytics]} \${BPT_ANALYTICS_CONFIG}
Restart=no
TimeoutStopSec=10

[Install]
WantedBy=bpt-dev-stack.target
EOF

# ── bpt-dev-bridge ───────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-dev-bridge.service" <<EOF
[Unit]
Description=BPT Dashboard Bridge (dev)
After=bpt-dev-strategy.service
Requires=bpt-dev-transport.service
PartOf=bpt-dev-stack.target

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT
ExecStart=${BIN[bpt-bridge]} --config \${BPT_BRIDGE_CONFIG} --mode \${BPT_BRIDGE_MODE} --strategy-name \${BPT_BRIDGE_STRATEGY} --symbol \${BPT_BRIDGE_SYMBOL} --exchange \${BPT_BRIDGE_EXCHANGE} --instrument-type \${BPT_BRIDGE_INST_TYPE}
Restart=no
TimeoutStopSec=10

[Install]
WantedBy=bpt-dev-stack.target
EOF

# ── bpt-dev-stack target ─────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-dev-stack.target" <<EOF
[Unit]
Description=BPT Trading Stack (dev — Bazel-built, isolated from prod)
Wants=bpt-dev-transport.service bpt-dev-refdata.service bpt-dev-md-gateway.service bpt-dev-order-gateway.service bpt-dev-strategy.service bpt-dev-analytics.service bpt-dev-bridge.service

[Install]
WantedBy=default.target
EOF

systemctl --user daemon-reload
echo "Generated $(ls "$UNIT_DIR"/bpt-dev-* | wc -l) dev units in $UNIT_DIR"
echo "Dev env: $ENV_FILE"
echo
echo "Before starting the first time:"
echo "  1. bazel build //..."
echo "  2. Create $BPT_ROOT/transport/aeron/config/config-dev.yaml"
echo "     (copy from config.yaml, change the aeron.dir to /dev/shm/aeron-bpt-dev)"
echo "  3. Ensure $ENV_FILE exists (see deploy/env/dev.env template)"
echo
echo "Usage:"
echo "  systemctl --user start bpt-dev-stack.target    # start dev stack"
echo "  systemctl --user stop  bpt-dev-stack.target    # stop"
echo "  systemctl --user status 'bpt-dev-*'            # check all"
echo "  journalctl --user -u bpt-dev-strategy -f       # tail strategy logs"

#!/bin/bash
# generate-units.sh — Generate systemd user units for the BPT trading stack.
#
# Runs in two modes:
#
#   1. LAPTOP (default)
#      - BPT_DEPLOY_ROOT unset
#      - Binaries from $BPT_ROOT/bazel-bin/... (Bazel's nested output layout)
#      - Configs / scripts / jar from the source checkout
#      - Writes units to ~/.config/systemd/user/
#      - Used by developers running the stack from their git checkout.
#
#   2. DEPLOY (set BPT_DEPLOY_ROOT=/opt/bpt)
#      - Binaries from $BPT_DEPLOY_ROOT/current/bin/... (flat layout from tarball)
#      - Configs from $BPT_DEPLOY_ROOT/config/active/
#      - Scripts from $BPT_DEPLOY_ROOT/current/scripts/
#      - Jar from $BPT_DEPLOY_ROOT/current/bin/transport/bpt-transport-*.jar
#      - Writes units to $BPT_UNIT_DIR (defaults to /etc/bpt/systemd or ~/.config/systemd/user)
#      - Run from deploy.sh during release swap.
#
# The config-sync timer is also emitted — it git-pulls the repo daily (laptop mode)
# or the config dir (deploy mode) so config PRs propagate to running services on
# their next refresh tick.
set -euo pipefail

# ── Mode + path resolution ──────────────────────────────────────────────────
if [ -n "${BPT_DEPLOY_ROOT:-}" ]; then
    # Deploy mode — /opt/bpt/-style layout
    BPT_ROOT="$BPT_DEPLOY_ROOT"
    BPT_BIN_ROOT="$BPT_DEPLOY_ROOT/current/bin"
    BPT_JAR_DIR="$BPT_DEPLOY_ROOT/current/bin/transport"
    # Deploy ships both scripts flat under current/scripts/
    SYNC_CONFIG_SCRIPT="$BPT_DEPLOY_ROOT/current/scripts/sync-config.sh"
    LOG_CLEANUP_SCRIPT="$BPT_DEPLOY_ROOT/current/scripts/cleanup-stale-logs.sh"
    BPT_CONFIG_DIR="$BPT_DEPLOY_ROOT/config/active"
    # Host env file for active env — produced during deploy, not committed
    ENV_FILE="$BPT_CONFIG_DIR/env"
    UNIT_DIR="${BPT_UNIT_DIR:-$HOME/.config/systemd/user}"
else
    # Laptop mode — source checkout with Bazel outputs in-tree
    BPT_ROOT="${BPT_ROOT:-/home/jseow/code/bpt-core}"
    BPT_BIN_ROOT="$BPT_ROOT/bazel-bin"
    BPT_JAR_DIR="$BPT_ROOT/transport/aeron/build/libs"
    # Laptop: sync-config lives under deploy/, cleanup under scripts/
    # (split is historical; tarball flattens both into scripts/)
    SYNC_CONFIG_SCRIPT="$BPT_ROOT/deploy/sync-config.sh"
    LOG_CLEANUP_SCRIPT="$BPT_ROOT/scripts/cleanup-stale-logs.sh"
    BPT_CONFIG_DIR="$BPT_ROOT"
    ENV_FILE="$BPT_ROOT/deploy/env/active.env"
    UNIT_DIR="${BPT_UNIT_DIR:-$HOME/.config/systemd/user}"
fi

mkdir -p "$UNIT_DIR"

# ── Binary paths ────────────────────────────────────────────────────────────
# Bazel output: $BPT_BIN_ROOT/<target>/<binary>       (nested, laptop)
# Tarball output: $BPT_BIN_ROOT/<binary>              (flat, deploy)
declare -A BIN
if [ -n "${BPT_DEPLOY_ROOT:-}" ]; then
    BIN[bpt-refdata]="$BPT_BIN_ROOT/bpt-refdata"
    BIN[bpt-md-gateway]="$BPT_BIN_ROOT/bpt-md-gateway"
    BIN[bpt-order-gateway]="$BPT_BIN_ROOT/bpt-order-gateway"
    BIN[bpt-strategy]="$BPT_BIN_ROOT/bpt-strategy"
    BIN[bpt-analytics]="$BPT_BIN_ROOT/bpt-analytics"
    BIN[bpt-book]="$BPT_BIN_ROOT/bpt-book"
    BIN[bpt-bridge]="$BPT_BIN_ROOT/bridge"
else
    BIN[bpt-refdata]="$BPT_BIN_ROOT/bpt-refdata/bpt-refdata"
    BIN[bpt-md-gateway]="$BPT_BIN_ROOT/bpt-md-gateway/bpt-md-gateway"
    BIN[bpt-order-gateway]="$BPT_BIN_ROOT/bpt-order-gateway/bpt-order-gateway"
    BIN[bpt-strategy]="$BPT_BIN_ROOT/bpt-strategy/bpt-strategy"
    BIN[bpt-analytics]="$BPT_BIN_ROOT/bpt-analytics/bpt-analytics"
    BIN[bpt-book]="$BPT_BIN_ROOT/bpt-book/bpt-book"
    BIN[bpt-bridge]="$BPT_BIN_ROOT/dashboard/bridge/bridge"
fi

# Java jar — different filename convention between the two modes
# (laptop: unversioned name from gradle, deploy: version-suffixed).
TRANSPORT_JAR=$(ls "$BPT_JAR_DIR"/bpt-transport-*-all.jar 2>/dev/null | head -1)
if [ -z "$TRANSPORT_JAR" ]; then
    echo "ERROR: no bpt-transport-*-all.jar under $BPT_JAR_DIR" >&2
    echo "       (laptop: run gradle shadowJar; deploy: tarball missing the jar)" >&2
    exit 1
fi

# Transport YAML — baked into the release tarball on deploy, in the
# gradle project tree on laptop.
if [ -n "${BPT_DEPLOY_ROOT:-}" ]; then
    TRANSPORT_CONFIG="$BPT_DEPLOY_ROOT/current/share/transport.yaml"
    TRANSPORT_WORKDIR="$BPT_DEPLOY_ROOT"
else
    TRANSPORT_CONFIG="$BPT_ROOT/transport/aeron/config/config.yaml"
    TRANSPORT_WORKDIR="$BPT_ROOT/transport"
fi

# ── bpt-transport (Aeron media driver, Gradle-built Java) ───────────────────
cat > "$UNIT_DIR/bpt-transport.service" <<EOF
[Unit]
Description=BPT Transport (Aeron MediaDriver)
PartOf=bpt-stack.target

[Service]
Type=simple
WorkingDirectory=$TRANSPORT_WORKDIR
ExecStart=/usr/bin/java --add-opens java.base/sun.nio.ch=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED -jar $TRANSPORT_JAR --config $TRANSPORT_CONFIG
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

    case "$svc" in
        bpt-refdata)       svc_desc="Refdata" ;;
        bpt-md-gateway)    svc_desc="Market Data Gateway" ;;
        bpt-order-gateway) svc_desc="Order Gateway" ;;
        *)                 svc_desc="${svc#bpt-}" ;;
    esac

    cat > "$UNIT_DIR/$svc.service" <<EOF
[Unit]
Description=BPT $svc_desc
After=$after
Requires=bpt-transport.service
PartOf=bpt-stack.target bpt-transport.service

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
PartOf=bpt-stack.target bpt-transport.service

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

# ── bpt-book ─────────────────────────────────────────────────────────────────
# Read-only multi-venue balance / position aggregator. After refdata so
# exchange catalog is available; doesn't depend on md-gateway or strategy.
cat > "$UNIT_DIR/bpt-book.service" <<EOF
[Unit]
Description=BPT Book Service (multi-venue balance + position aggregator)
After=bpt-refdata.service
Requires=bpt-transport.service
PartOf=bpt-stack.target bpt-transport.service

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-book
ExecStart=${BIN[bpt-book]} --config \${BPT_BOOK_CONFIG}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10

[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-analytics ────────────────────────────────────────────────────────────
cat > "$UNIT_DIR/bpt-analytics.service" <<EOF
[Unit]
Description=BPT Analytics (markouts, toxicity, fill rate)
After=bpt-strategy.service
Requires=bpt-transport.service
PartOf=bpt-stack.target bpt-transport.service

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
PartOf=bpt-stack.target bpt-transport.service

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

# ── bpt-frontend (dashboard UI, Vite dev server) — LAPTOP ONLY ──────────────
# Vite dev server runs from the source checkout; not shipped in deploy tarballs.
# Deploy hosts serve the built dashboard via bridge or a separate nginx/caddy
# out of band; emitting a unit that points at $BPT_DEPLOY_ROOT/dashboard/frontend
# would produce a ghost unit that fails to start on every boot.
if [ -z "${BPT_DEPLOY_ROOT:-}" ]; then
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
fi

# ── bpt-config-sync.{service,timer} ─────────────────────────────────────────
# Pulls config changes (instrument mapping, fees, service TOMLs) from origin
# daily. Services pick up the new files on their next internal refresh tick;
# nothing restarts.
cat > "$UNIT_DIR/bpt-config-sync.service" <<EOF
[Unit]
Description=BPT Config Sync (git pull for config-only updates)

[Service]
Type=oneshot
ExecStart=$SYNC_CONFIG_SCRIPT
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

# ── bpt-heartbeat ──────────────────────────────────────────────────────────
# Prometheus + Alertmanager + Grafana all live in monitoring/docker-compose.yml
# — see monitoring/README.md.  The only piece still in systemd is the
# heartbeat timer that curls Healthchecks.io as a dead-man's-switch for the
# host-down case (which neither local Docker nor local Alertmanager can
# alert on by definition).

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

# ── bpt-log-cleanup.{service,timer} ─────────────────────────────────────────
# Weekly prune of log files not modified in 30+ days. Catches orphan
# log families left behind by service renames (quill rotates active
# logs but can't reap a series that's no longer being written to).
cat > "$UNIT_DIR/bpt-log-cleanup.service" <<EOF
[Unit]
Description=BPT stale log cleanup (>30d, untouched since rename)

[Service]
Type=oneshot
ExecStart=$LOG_CLEANUP_SCRIPT
Environment=BPT_ROOT=$BPT_ROOT
Environment=RETENTION_DAYS=30
EOF

cat > "$UNIT_DIR/bpt-log-cleanup.timer" <<EOF
[Unit]
Description=Weekly BPT log cleanup (Sunday 04:00)

[Timer]
OnCalendar=Sun *-*-* 04:00:00
Persistent=true
RandomizedDelaySec=15min

[Install]
WantedBy=timers.target
EOF

# ── bpt-stack.target ─────────────────────────────────────────────────────────
# Frontend unit only exists in laptop mode; keep it out of the target's
# Wants= in deploy mode so systemd doesn't try to start a ghost unit.
stack_wants="bpt-transport.service bpt-refdata.service bpt-md-gateway.service bpt-order-gateway.service bpt-strategy.service bpt-analytics.service bpt-book.service bpt-bridge.service bpt-heartbeat.timer"
if [ -z "${BPT_DEPLOY_ROOT:-}" ]; then
    stack_wants="$stack_wants bpt-frontend.service"
fi

cat > "$UNIT_DIR/bpt-stack.target" <<EOF
[Unit]
Description=BPT Trading Stack
Wants=$stack_wants

[Install]
WantedBy=default.target
EOF

# Only daemon-reload when we're writing into the user's live systemd dir.
# When BPT_UNIT_DIR is overridden (e.g. deploy.sh staging into a tmp dir
# before copying atomically), reloading would be a no-op with the wrong
# side effect of stopping-on-syntax-errors in the staging tree.
if [ "$UNIT_DIR" = "$HOME/.config/systemd/user" ]; then
    systemctl --user daemon-reload
fi

echo "Generated $(ls "$UNIT_DIR"/bpt-*.service "$UNIT_DIR"/bpt-*.timer "$UNIT_DIR"/bpt-*.target 2>/dev/null | wc -l) units in $UNIT_DIR"
echo "Active env: $(readlink -f "$ENV_FILE" 2>/dev/null || echo "$ENV_FILE")"
echo
echo "Enable + start:"
echo "  systemctl --user enable --now bpt-config-sync.timer   # daily config pull"
echo "  systemctl --user enable --now bpt-heartbeat.timer     # dead-man's-switch to Healthchecks.io"
echo "  systemctl --user start bpt-stack.target               # bring the stack up"
echo
echo "Monitoring setup (see monitoring/README.md):"
echo "  # ntfy.sh URL files (kept on host at ~/.config/bpt/, bind-mounted into Alertmanager container)"
echo "  mkdir -p ~/.config/bpt && chmod 0700 ~/.config/bpt"
echo "  TOPIC=\"bpt-alerts-\$(uuidgen | tr -d - | head -c 12)\""
echo "  echo -n \"https://ntfy.sh/\$TOPIC?priority=urgent&tags=rotating_light&title=BPT+CRITICAL\" > ~/.config/bpt/ntfy-url-critical"
echo "  echo -n \"https://ntfy.sh/\$TOPIC?priority=default&tags=warning&title=BPT+WARNING\"        > ~/.config/bpt/ntfy-url-warning"
echo "  chmod 0600 ~/.config/bpt/ntfy-url-critical ~/.config/bpt/ntfy-url-warning"
echo "  # Subscribe the phone app (Android/iOS) to \$TOPIC, then bring up the monitoring stack:"
echo "  cd monitoring && docker compose up -d"
echo "  echo 'HC_URL=https://hc-ping.com/YOUR-UUID' | sudo tee /etc/bpt/healthchecks.env"
echo
echo "Inspect:"
echo "  systemctl --user status 'bpt-*'"
echo "  systemctl --user list-timers bpt-config-sync.timer"
echo "  journalctl --user -u bpt-config-sync -f   # tail sync logs"

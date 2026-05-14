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
#
# Stack-wide registries every service reads at startup (per-service WorkingDirectory
# is set so each service's relative paths resolve):
#
#   deploy/config/aeron/streams.toml      — Aeron stream IDs + media driver dir.
#                                            Referenced by `aeron_config = "..."` in
#                                            each service's instance TOML.
#   deploy/config/profile/<tag>.toml      — Deployment profile (environment,
#                                            exchanges filter, exchange_config path).
#                                            Referenced by `profile_config = "..."`
#                                            in each service's instance TOML; bridge
#                                            takes the path via --profile CLI arg
#                                            (BPT_BRIDGE_PROFILE in the env file).
#
# switch-env.sh refuses to activate an env file whose picked configs disagree on
# profile_config — a structural defence against a stack booting with services on
# different exchanges.
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
    BPT_ROOT="${BPT_ROOT:-${HOME}/code/bpt-core}"
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
    BIN[bpt-tape]="$BPT_BIN_ROOT/bpt-tape"
    BIN[bpt-order-gateway]="$BPT_BIN_ROOT/bpt-order-gateway"
    BIN[bpt-strategy]="$BPT_BIN_ROOT/bpt-strategy"
    BIN[bpt-analytics]="$BPT_BIN_ROOT/bpt-analytics"
    BIN[bpt-pms]="$BPT_BIN_ROOT/bpt-pms"
    BIN[bpt-bridge]="$BPT_BIN_ROOT/bridge"
else
    BIN[bpt-refdata]="$BPT_BIN_ROOT/bpt-refdata/bpt-refdata"
    BIN[bpt-md-gateway]="$BPT_BIN_ROOT/bpt-md-gateway/bpt-md-gateway"
    BIN[bpt-tape]="$BPT_BIN_ROOT/bpt-tape/bpt-tape"
    BIN[bpt-order-gateway]="$BPT_BIN_ROOT/bpt-order-gateway/bpt-order-gateway"
    BIN[bpt-strategy]="$BPT_BIN_ROOT/bpt-strategy/bpt-strategy"
    BIN[bpt-analytics]="$BPT_BIN_ROOT/bpt-analytics/bpt-analytics"
    BIN[bpt-pms]="$BPT_BIN_ROOT/bpt-pms/bpt-pms"
    BIN[bpt-bridge]="$BPT_BIN_ROOT/bpt-bridge/bpt-bridge"
fi

# ── Resource limits (per-service, tiered by trading criticality) ────────────
# Three tiers:
#   hotpath  — must survive memory pressure (transport, md-gw, ogw, strategy).
#              OOMScoreAdjust=-100 means kernel kills these last.
#   support  — required for trading but off the order/MD path (refdata).
#              OOMScoreAdjust=-50.
#   aux      — observability/recording, kill first (analytics, book, bridge,
#              tape). OOMScoreAdjust=0 (default).
#
# MemoryMax caps are deliberately generous (~3-5x measured steady-state on
# OKX demo) — the goal is "stop a runaway" not "shrink-wrap normal use".
# TasksMax catches thread-leak fork bombs. LimitNOFILE budgets fds for WS
# fanout (each adapter holds REST + WS sockets per venue + Aeron pubs/subs).
declare -A MEM TASKS NOFILE OOMADJ
MEM[bpt-transport]=4G;       TASKS[bpt-transport]=512;       NOFILE[bpt-transport]=65536; OOMADJ[bpt-transport]=-100
MEM[bpt-md-gateway]=1G;      TASKS[bpt-md-gateway]=256;      NOFILE[bpt-md-gateway]=16384; OOMADJ[bpt-md-gateway]=-100
MEM[bpt-order-gateway]=1G;   TASKS[bpt-order-gateway]=256;   NOFILE[bpt-order-gateway]=8192; OOMADJ[bpt-order-gateway]=-100
MEM[bpt-strategy]=1G;        TASKS[bpt-strategy]=128;        NOFILE[bpt-strategy]=4096; OOMADJ[bpt-strategy]=-100
MEM[bpt-refdata]=512M;       TASKS[bpt-refdata]=128;         NOFILE[bpt-refdata]=4096; OOMADJ[bpt-refdata]=-50
MEM[bpt-pms]=256M;          TASKS[bpt-pms]=64;             NOFILE[bpt-pms]=4096; OOMADJ[bpt-pms]=0
MEM[bpt-analytics]=512M;     TASKS[bpt-analytics]=128;       NOFILE[bpt-analytics]=4096; OOMADJ[bpt-analytics]=0
MEM[bpt-bridge]=256M;        TASKS[bpt-bridge]=64;           NOFILE[bpt-bridge]=4096; OOMADJ[bpt-bridge]=0
MEM[bpt-tape]=2G;            TASKS[bpt-tape]=128;            NOFILE[bpt-tape]=8192; OOMADJ[bpt-tape]=0

# Restart-loop guard: 5 failed starts inside 300s → systemd gives up instead
# of looping forever. Pairs with the existing Restart=on-failure RestartSec=3.
START_LIMIT_INTERVAL=300
START_LIMIT_BURST=5

# Emit the per-service [Service] resource block. Caller passes the service
# name; the function looks up the tier values and prints them. Trailing
# newline is intentional — caller embeds the result mid-heredoc.
emit_limits() {
    local svc=$1
    cat <<EOF
MemoryMax=${MEM[$svc]}
TasksMax=${TASKS[$svc]}
LimitNOFILE=${NOFILE[$svc]}
OOMScoreAdjust=${OOMADJ[$svc]}
EOF
}

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
StartLimitIntervalSec=$START_LIMIT_INTERVAL
StartLimitBurst=$START_LIMIT_BURST

[Service]
Type=simple
WorkingDirectory=$TRANSPORT_WORKDIR
ExecStart=/usr/bin/java --add-opens java.base/sun.nio.ch=ALL-UNNAMED --add-opens java.base/java.nio=ALL-UNNAMED -jar $TRANSPORT_JAR --config $TRANSPORT_CONFIG
Restart=on-failure
RestartSec=3
TimeoutStopSec=10
$(emit_limits bpt-transport)
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
StartLimitIntervalSec=$START_LIMIT_INTERVAL
StartLimitBurst=$START_LIMIT_BURST

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/$svc
ExecStart=${BIN[$svc]} --config \${$cfg_var}
${creds_lines}Restart=on-failure
RestartSec=3
TimeoutStopSec=15
$(emit_limits "$svc")
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
StartLimitIntervalSec=$START_LIMIT_INTERVAL
StartLimitBurst=$START_LIMIT_BURST

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-strategy
ExecStart=${BIN[bpt-strategy]} --config \${BPT_STRATEGY_CONFIG}
Restart=on-failure
RestartSec=5
TimeoutStopSec=30
$(emit_limits bpt-strategy)
[Install]
WantedBy=bpt-stack.target
EOF

# ── bpt-pms ─────────────────────────────────────────────────────────────────
# Read-only multi-venue balance / position aggregator. After refdata so
# exchange catalog is available; doesn't depend on md-gateway or strategy.
cat > "$UNIT_DIR/bpt-pms.service" <<EOF
[Unit]
Description=BPT Book Service (multi-venue balance + position aggregator)
After=bpt-refdata.service
Requires=bpt-transport.service
PartOf=bpt-stack.target bpt-transport.service
StartLimitIntervalSec=$START_LIMIT_INTERVAL
StartLimitBurst=$START_LIMIT_BURST

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-pms
ExecStart=${BIN[bpt-pms]} --config \${BPT_PMS_CONFIG}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10
$(emit_limits bpt-pms)
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
StartLimitIntervalSec=$START_LIMIT_INTERVAL
StartLimitBurst=$START_LIMIT_BURST

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-analytics
ExecStart=${BIN[bpt-analytics]} --config \${BPT_ANALYTICS_CONFIG}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10
$(emit_limits bpt-analytics)
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
StartLimitIntervalSec=$START_LIMIT_INTERVAL
StartLimitBurst=$START_LIMIT_BURST

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT
ExecStart=${BIN[bpt-bridge]} --config \${BPT_BRIDGE_CONFIG} --profile \${BPT_BRIDGE_PROFILE} --mode \${BPT_BRIDGE_MODE} --strategy-name \${BPT_BRIDGE_STRATEGY} --symbol \${BPT_BRIDGE_SYMBOL} --exchange \${BPT_BRIDGE_EXCHANGE} --instrument-type \${BPT_BRIDGE_INST_TYPE}
Restart=on-failure
RestartSec=3
TimeoutStopSec=10
$(emit_limits bpt-bridge)
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
Environment=PATH=${HOME}/.nvm/versions/node/v20.20.1/bin:/usr/bin
ExecStart=/usr/bin/env node $BPT_ROOT/dashboard/frontend/node_modules/.bin/vite
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

# ── bpt-tape.service + recording rotate timer ──────────────────────────────
# bpt-tape ("the tape") is a dedicated binary that imports bpt-md-gateway's
# adapter library and substitutes recording-aware subclasses (override
# on_frame to tee raw WS bytes to disk via Tape). Trading-stack
# mdgw + refdata services have no recording code — recording lives
# entirely in this process. Designed for a recording-only VPS that
# just sits there capturing.
#
# Config path comes from $BPT_TAPE_CONFIG in the active env
# (typically points at bpt-tape.hl.toml). Output dir is
# /opt/bpt/data/raw/<venue>/<date>/, set inside the TOML — same layout
# wslog_to_parquet.py expects.
cat > "$UNIT_DIR/bpt-tape.service" <<EOF
[Unit]
Description=BPT Tape (continuous WS capture)
PartOf=bpt-recording.target
StartLimitIntervalSec=$START_LIMIT_INTERVAL
StartLimitBurst=$START_LIMIT_BURST

[Service]
Type=simple
EnvironmentFile=$ENV_FILE
WorkingDirectory=$BPT_ROOT/bpt-tape
ExecStart=${BIN[bpt-tape]} --config \${BPT_TAPE_CONFIG}
Restart=always
RestartSec=5
TimeoutStopSec=15
$(emit_limits bpt-tape)
[Install]
WantedBy=bpt-recording.target
EOF

# Daily wslog → Parquet conversion. Runs at 00:30 UTC for the
# previous UTC day's files (matches record-stack.sh's UTC dating
# convention). Idempotent — re-runs on the same day overwrite the
# same Parquet rows. Scope-limited to the recording host: never
# fires on trading-stack hosts because the recording.target on
# them is empty.
ROTATE_SCRIPT_LAPTOP="$BPT_ROOT/scripts/rotate_recordings.sh"
ROTATE_SCRIPT_DEPLOY="\$BPT_ROOT/current/scripts/rotate_recordings.sh"
if [ -n "${BPT_DEPLOY_ROOT:-}" ]; then
    rotate_exec="$BPT_DEPLOY_ROOT/current/scripts/rotate_recordings.sh"
else
    rotate_exec="$ROTATE_SCRIPT_LAPTOP"
fi

cat > "$UNIT_DIR/bpt-recording-rotate.service" <<EOF
[Unit]
Description=BPT recording rotate (yesterday's wslogs → Parquet)

[Service]
Type=oneshot
EnvironmentFile=$ENV_FILE
ExecStart=$rotate_exec
EOF

cat > "$UNIT_DIR/bpt-recording-rotate.timer" <<EOF
[Unit]
Description=Daily BPT recording rotate (00:30 UTC)
PartOf=bpt-recording.target

[Timer]
OnCalendar=*-*-* 00:30:00 UTC
Persistent=true
RandomizedDelaySec=5min

[Install]
WantedBy=bpt-recording.target
EOF

# Recorder dead-man's-switch: not "host is alive" (the trading-stack
# bpt-heartbeat covers that for trading hosts) but "the recorder is
# actually capturing frames". The script verifies the service is
# active AND the latest .wslog mtime is fresh — silent WS death is
# the failure mode this catches.
if [ -n "${BPT_DEPLOY_ROOT:-}" ]; then
    health_exec="$BPT_DEPLOY_ROOT/current/scripts/check_recording_health.sh"
else
    health_exec="$BPT_ROOT/scripts/check_recording_health.sh"
fi

cat > "$UNIT_DIR/bpt-recording-heartbeat.service" <<EOF
[Unit]
Description=BPT recorder freshness check + Healthchecks.io ping

[Service]
Type=oneshot
EnvironmentFile=/etc/bpt/healthchecks.env
ExecStart=$health_exec
EOF

cat > "$UNIT_DIR/bpt-recording-heartbeat.timer" <<EOF
[Unit]
Description=BPT recorder heartbeat every 5 min
PartOf=bpt-recording.target

[Timer]
OnBootSec=2min
OnUnitActiveSec=5min
AccuracySec=30s

[Install]
WantedBy=bpt-recording.target
EOF

# Hourly tape sync: pushes raw .wslog + parquet to S3 archive bucket.
# Closes the durability hole between the daily rotate (Parquet conversion)
# and the worst-case window where the recording host disk dies before
# yesterday's data has been replicated off-host. See scripts/sync_tape_to_s3.sh.
if [ -n "${BPT_DEPLOY_ROOT:-}" ]; then
    sync_exec="$BPT_DEPLOY_ROOT/current/scripts/sync_tape_to_s3.sh"
else
    sync_exec="$BPT_ROOT/scripts/sync_tape_to_s3.sh"
fi

cat > "$UNIT_DIR/bpt-tape-sync.service" <<EOF
[Unit]
Description=BPT tape → S3 archive sync (hourly)

[Service]
Type=oneshot
EnvironmentFile=$ENV_FILE
ExecStart=$sync_exec
EOF

cat > "$UNIT_DIR/bpt-tape-sync.timer" <<EOF
[Unit]
Description=BPT tape sync every hour
PartOf=bpt-recording.target

[Timer]
OnBootSec=10min
OnUnitActiveSec=1h
AccuracySec=5min

[Install]
WantedBy=bpt-recording.target
EOF

cat > "$UNIT_DIR/bpt-recording.target" <<EOF
[Unit]
Description=BPT Recording Stack (tape + daily Parquet rotate + S3 sync + heartbeat)
Wants=bpt-tape.service bpt-recording-rotate.timer bpt-recording-heartbeat.timer bpt-tape-sync.timer

[Install]
WantedBy=default.target
EOF

# ── bpt-stack.target ─────────────────────────────────────────────────────────
# Frontend unit only exists in laptop mode; keep it out of the target's
# Wants= in deploy mode so systemd doesn't try to start a ghost unit.
stack_wants="bpt-transport.service bpt-refdata.service bpt-md-gateway.service bpt-order-gateway.service bpt-strategy.service bpt-analytics.service bpt-pms.service bpt-bridge.service bpt-heartbeat.timer"
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

# Resource-limit cgroup delegation check — MemoryMax/TasksMax/etc on user
# units are silent no-ops unless the system instance has delegated the
# memory + pids controllers down. Confirm by checking the property on the
# user manager; if the controllers aren't delegated, point the operator at
# the one-time fix.
if [ "$UNIT_DIR" = "$HOME/.config/systemd/user" ] && command -v systemctl >/dev/null; then
    delegate_controllers=$(systemctl --user show -p DelegateControllers --value 2>/dev/null || echo "")
    if [[ "$delegate_controllers" != *memory* ]] || [[ "$delegate_controllers" != *pids* ]]; then
        echo
        echo "WARNING: user-instance cgroup delegation is missing memory and/or pids controllers."
        echo "         MemoryMax / TasksMax in these units will be silent no-ops until you run"
        echo "         (one-time, requires root):"
        echo "             sudo mkdir -p /etc/systemd/system/user@.service.d"
        echo "             sudo tee /etc/systemd/system/user@.service.d/delegate.conf <<'C'"
        echo "             [Service]"
        echo "             Delegate=memory pids cpu io"
        echo "             C"
        echo "             sudo systemctl daemon-reload"
        echo "             sudo systemctl restart user@\$(id -u).service   # logs you out of the user scope"
    fi
fi
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

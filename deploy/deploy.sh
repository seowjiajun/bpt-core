#!/bin/bash
# deploy.sh — unpack a BPT release tarball into $BPT_DEPLOY_ROOT, flip the
# current/ symlink atomically, seed per-service config dirs on first install,
# regenerate systemd units, and (optionally) restart the stack.
#
# Usage:
#   BPT_DEPLOY_ROOT=/opt/bpt ./deploy.sh dist/bpt-<ver>.tar.gz [--restart]
#   BPT_DEPLOY_ROOT=/opt/bpt ./deploy.sh dist/bpt-<ver>.tar.gz --dry-run
#
# Required:
#   BPT_DEPLOY_ROOT   target root (e.g. /opt/bpt, $HOME/bpt-deploy). Must exist.
#
# Optional env:
#   BPT_UNIT_DIR      where to install units. Default: $HOME/.config/systemd/user.
#                     Override on test hosts that already host a different stack
#                     you don't want to clobber.
#
# Flags:
#   --restart         after unit regen, stop → daemon-reload → start bpt-stack.target.
#                     Default is stage units only + print the restart commands, so
#                     the operator chooses when to cut over.
#   --dry-run         print the plan, don't write anything.
#
# Layout produced (matches deploy/generate-units.sh deploy-mode assumptions):
#
#   $BPT_DEPLOY_ROOT/
#   ├── current         → releases/<ver>/         (atomically flipped)
#   ├── previous        → releases/<old-ver>/     (for rollback.sh)
#   ├── releases/<ver>/{bin,scripts,share,VERSION,MANIFEST,...}
#   ├── bpt-<svc>/config/{*.toml,exchanges/,...}  (seeded once, left alone on upgrade)
#   ├── dashboard/bridge/config/bridge.live.toml
#   ├── config/instruments → current/share/instruments/   (symlink, auto-follows current)
#   ├── config/active/env                                 (operator-staged; deploy refuses to start without it)
#
# First-install vs upgrade:
#   First install — operator creates BPT_DEPLOY_ROOT, deploy.sh untars the first
#   release, seeds bpt-<svc>/config/ from share/service-configs/, bails with
#   instructions for staging config/active/env. Operator fills in env, re-runs.
#   Upgrade — deploy.sh sees bpt-<svc>/config/ already exists; leaves them alone
#   so operator-edited overrides survive.

set -euo pipefail
shopt -s nullglob

# ── Arg parse ───────────────────────────────────────────────────────────────
TARBALL=""
RESTART=0
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --restart)  RESTART=1 ;;
        --dry-run)  DRY_RUN=1 ;;
        --help|-h)  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'; exit 0 ;;
        -*)         echo "deploy.sh: unknown flag: $arg" >&2; exit 2 ;;
        *)          TARBALL="$arg" ;;
    esac
done

if [ -z "$TARBALL" ]; then
    echo "deploy.sh: tarball path required. Try --help." >&2
    exit 2
fi
if [ ! -f "$TARBALL" ]; then
    echo "deploy.sh: tarball not found: $TARBALL" >&2
    exit 2
fi
if [ -z "${BPT_DEPLOY_ROOT:-}" ]; then
    echo "deploy.sh: BPT_DEPLOY_ROOT env var must be set (e.g. /opt/bpt)" >&2
    exit 2
fi
if [ ! -d "$BPT_DEPLOY_ROOT" ]; then
    echo "deploy.sh: BPT_DEPLOY_ROOT does not exist: $BPT_DEPLOY_ROOT" >&2
    echo "           create it first (mkdir -p) — we don't auto-create" \
         "outside the user's explicit choice." >&2
    exit 2
fi

# ── Logging ─────────────────────────────────────────────────────────────────
log() { echo "[deploy $(date +%H:%M:%S)] $*"; }
run() {
    if [ "$DRY_RUN" = "1" ]; then
        echo "DRY-RUN: $*"
    else
        eval "$@"
    fi
}

# ── Resolve version from tarball ────────────────────────────────────────────
# Tarball top-level is a single dir `bpt-<ver>/`. Extract <ver> from there
# rather than the filename so operator-renamed tarballs still work.
# `head -1` closes the pipe after one read → tar gets SIGPIPE → pipefail
# would kill us; disable pipefail just for this resolution.
set +o pipefail
TOPDIR=$(tar -tzf "$TARBALL" | head -1 | tr -d '/')
set -o pipefail
if [[ ! "$TOPDIR" =~ ^bpt-v[0-9] ]]; then
    echo "deploy.sh: tarball top-level dir doesn't look like bpt-<ver>/: $TOPDIR" >&2
    exit 3
fi
VERSION="${TOPDIR#bpt-}"
log "Deploying $VERSION from $TARBALL to $BPT_DEPLOY_ROOT"

RELEASE_DIR="$BPT_DEPLOY_ROOT/releases/$VERSION"
CURRENT_LINK="$BPT_DEPLOY_ROOT/current"
PREVIOUS_LINK="$BPT_DEPLOY_ROOT/previous"

SKIP_UNTAR=0
if [ -d "$RELEASE_DIR" ]; then
    # Release already on disk. Two cases:
    #   (a) current/ already points at it — deploy was interrupted mid-flow
    #       (e.g. env-file bail). Resume by regenerating units; skip untar.
    #   (b) current/ points somewhere else — operator is trying to re-deploy
    #       a release they already have staged. Refuse; force explicit cleanup.
    if [ -L "$CURRENT_LINK" ] && [ "$(readlink "$CURRENT_LINK")" = "releases/$VERSION" ]; then
        log "Release $VERSION already on disk AND current/; resuming (skip untar)."
        SKIP_UNTAR=1
    else
        log "Release dir $RELEASE_DIR already exists but current/ points elsewhere."
        log "Rollback to it via rollback.sh, or rm -rf releases/$VERSION to re-deploy a dirty build."
        exit 4
    fi
fi

# ── Untar ───────────────────────────────────────────────────────────────────
if [ "$SKIP_UNTAR" = "0" ]; then
    log "Untarring into $BPT_DEPLOY_ROOT/releases/"
    run "mkdir -p '$BPT_DEPLOY_ROOT/releases'"
    run "tar -xzf '$TARBALL' -C '$BPT_DEPLOY_ROOT/releases'"
    if [ "$DRY_RUN" = "0" ]; then
        mv "$BPT_DEPLOY_ROOT/releases/$TOPDIR" "$RELEASE_DIR"
    fi
fi

# ── First-install detection + seeding ───────────────────────────────────────
# Treat "no existing current symlink" as first install.
FIRST_INSTALL=0
if [ ! -L "$CURRENT_LINK" ]; then
    FIRST_INSTALL=1
    log "No existing current/ — first install on this host."
fi

if [ "$FIRST_INSTALL" = "1" ]; then
    log "Seeding per-service config dirs from release share/service-configs/"
    for svc in refdata md-gateway order-gateway strategy analytics; do
        src="$RELEASE_DIR/share/service-configs/$svc"
        dst="$BPT_DEPLOY_ROOT/bpt-$svc/config"
        if [ ! -d "$dst" ]; then
            run "mkdir -p '$dst'"
            run "cp -r '$src'/. '$dst/'"
            log "  seeded bpt-$svc/config/"
        fi
    done
    # Bridge lives under dashboard/bridge/config, not bpt-bridge/
    if [ ! -d "$BPT_DEPLOY_ROOT/dashboard/bridge/config" ]; then
        run "mkdir -p '$BPT_DEPLOY_ROOT/dashboard/bridge/config'"
        run "cp -r '$RELEASE_DIR/share/service-configs/bridge'/. '$BPT_DEPLOY_ROOT/dashboard/bridge/config/'"
        log "  seeded dashboard/bridge/config/"
    fi

    # config/instruments → current/share/instruments (relative ../ refs in service TOMLs).
    # Symlink, not copy, so upgrades automatically pick up new mapping JSONs.
    run "mkdir -p '$BPT_DEPLOY_ROOT/config/active'"
    if [ ! -e "$BPT_DEPLOY_ROOT/config/instruments" ]; then
        run "ln -sfn '$CURRENT_LINK/share/instruments' '$BPT_DEPLOY_ROOT/config/instruments'"
        log "  linked config/instruments → current/share/instruments"
    fi
fi

# ── Atomic current/ flip ────────────────────────────────────────────────────
# Record previous for rollback BEFORE flipping. On first install, previous
# stays absent; rollback.sh bails with a clear error.
if [ -L "$CURRENT_LINK" ]; then
    old_target=$(readlink "$CURRENT_LINK")
    log "Recording previous: $old_target"
    run "ln -sfn '$old_target' '$PREVIOUS_LINK'"
fi

log "Flipping current/ → releases/$VERSION"
# ln -sfn + mv -T pattern: ln overwrites the symlink atomically on Linux
# (same-dir rename), so either old or new is always visible — no torn state.
run "ln -sfn 'releases/$VERSION' '$CURRENT_LINK.new' && mv -T '$CURRENT_LINK.new' '$CURRENT_LINK'"

# ── Env file guard ──────────────────────────────────────────────────────────
ENV_FILE="$BPT_DEPLOY_ROOT/config/active/env"
if [ ! -f "$ENV_FILE" ] && [ "$DRY_RUN" = "0" ]; then
    cat >&2 <<EOF

[deploy] Release unpacked and current/ flipped, but $ENV_FILE
         doesn't exist yet. Services won't start without it.

         Stage it (example from the repo's deploy/env/dev-hyperliquid.env):

             cp <your-host-env> $ENV_FILE
             chmod 0644 $ENV_FILE

         Then re-run deploy.sh to regenerate units + (optional) restart.
EOF
    exit 5
fi

# ── Regenerate units ────────────────────────────────────────────────────────
# Use a staging dir so generate-units.sh's trailing daemon-reload becomes
# a no-op (it only reloads when UNIT_DIR == user's live dir). We then
# copy atomically into place below.
STAGING_UNIT_DIR=$(mktemp -d -t bpt-deploy-units-XXXX)
LIVE_UNIT_DIR="${BPT_UNIT_DIR:-$HOME/.config/systemd/user}"

log "Regenerating systemd units via release's generate-units.sh"
run "BPT_DEPLOY_ROOT='$BPT_DEPLOY_ROOT' BPT_UNIT_DIR='$STAGING_UNIT_DIR' bash '$RELEASE_DIR/scripts/generate-units.sh' >/dev/null"

# Validate before we touch live units — unit syntax errors should fail
# the deploy rather than land broken units in the user's systemd dir.
if [ "$DRY_RUN" = "0" ]; then
    if ! systemd-analyze verify --user "$STAGING_UNIT_DIR"/*.service 2>&1; then
        log "systemd-analyze verify FAILED on staged units; aborting without touching live units."
        rm -rf "$STAGING_UNIT_DIR"
        exit 6
    fi
fi

# ── Unit swap ───────────────────────────────────────────────────────────────
if [ "$RESTART" = "1" ]; then
    log "Stopping bpt-stack.target (for --restart)"
    run "systemctl --user stop bpt-stack.target || true"
fi

log "Copying units from staging to $LIVE_UNIT_DIR"
run "mkdir -p '$LIVE_UNIT_DIR'"
run "cp -f '$STAGING_UNIT_DIR'/bpt-*.service '$STAGING_UNIT_DIR'/bpt-*.timer '$STAGING_UNIT_DIR'/bpt-*.target '$LIVE_UNIT_DIR/'"
rm -rf "$STAGING_UNIT_DIR"

if [ "$LIVE_UNIT_DIR" = "$HOME/.config/systemd/user" ]; then
    log "systemctl --user daemon-reload"
    run "systemctl --user daemon-reload"
else
    log "BPT_UNIT_DIR override active ($LIVE_UNIT_DIR) — skipping daemon-reload."
    log "Operator must reload manually if these units live under a real systemd dir."
fi

if [ "$RESTART" = "1" ]; then
    log "Starting bpt-stack.target"
    run "systemctl --user start bpt-stack.target"
    sleep 3
    log "Post-start status:"
    run "systemctl --user is-active bpt-transport.service bpt-refdata.service bpt-md-gateway.service bpt-order-gateway.service bpt-strategy.service bpt-analytics.service bpt-bridge.service || true"
fi

# ── Done ────────────────────────────────────────────────────────────────────
log ""
log "=== Deploy $VERSION complete ==="
if [ "$RESTART" = "0" ]; then
    log "Units staged but NOT restarted. When ready:"
    log "  systemctl --user restart bpt-stack.target"
    log "  systemctl --user status 'bpt-*'"
fi
log "Rollback target: $(readlink "$PREVIOUS_LINK" 2>/dev/null || echo '(none — first install)')"

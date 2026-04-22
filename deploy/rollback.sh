#!/bin/bash
# rollback.sh — flip current/ back to previous/ after a failed or regretted deploy.
#
# Usage:
#   BPT_DEPLOY_ROOT=/opt/bpt ./rollback.sh [--restart]
#
# rollback.sh does NOT remove the failed release from releases/ — leaves it
# there for forensics, operator can rm -rf after. Rolls back exactly one
# generation (current ↔ previous), not arbitrary history; repeat invocations
# would just oscillate between the same two, which is usually what you want
# in an incident but never what you want in a planned migration.
#
# Assumes deploy.sh was used to install the current release (otherwise there's
# no previous/ symlink to flip to).

set -euo pipefail

RESTART=0
for arg in "$@"; do
    case "$arg" in
        --restart)  RESTART=1 ;;
        --help|-h)  sed -n '2,/^$/p' "$0" | sed 's/^# \?//'; exit 0 ;;
        *)          echo "rollback.sh: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

if [ -z "${BPT_DEPLOY_ROOT:-}" ]; then
    echo "rollback.sh: BPT_DEPLOY_ROOT env var must be set" >&2
    exit 2
fi

CURRENT_LINK="$BPT_DEPLOY_ROOT/current"
PREVIOUS_LINK="$BPT_DEPLOY_ROOT/previous"

if [ ! -L "$CURRENT_LINK" ]; then
    echo "rollback.sh: no current/ symlink at $CURRENT_LINK — nothing to roll back." >&2
    exit 3
fi
if [ ! -L "$PREVIOUS_LINK" ]; then
    echo "rollback.sh: no previous/ symlink at $PREVIOUS_LINK." >&2
    echo "             Either this was the first install, or a prior rollback already consumed it." >&2
    exit 3
fi

cur_target=$(readlink "$CURRENT_LINK")
prev_target=$(readlink "$PREVIOUS_LINK")

if [ "$cur_target" = "$prev_target" ]; then
    echo "rollback.sh: current and previous point at the same release ($cur_target)." >&2
    echo "             Already rolled back; there's nothing further to do." >&2
    exit 3
fi

log() { echo "[rollback $(date +%H:%M:%S)] $*"; }

log "Rolling back: current=$cur_target → $prev_target"

# Swap: current ← previous-target, previous ← current-target (so next
# rollback would flip back the other way — useful if the rollback itself
# was a mistake).
log "Swapping current/ and previous/ symlinks"
ln -sfn "$prev_target" "$CURRENT_LINK.new" && mv -T "$CURRENT_LINK.new" "$CURRENT_LINK"
ln -sfn "$cur_target"  "$PREVIOUS_LINK.new" && mv -T "$PREVIOUS_LINK.new" "$PREVIOUS_LINK"

# Regenerate units from the now-current release's generate-units.sh.
# Using the rolled-back release's script matters: the old script knew its
# own layout, which is exactly what should run now.
STAGING_UNIT_DIR=$(mktemp -d -t bpt-rollback-units-XXXX)
LIVE_UNIT_DIR="${BPT_UNIT_DIR:-$HOME/.config/systemd/user}"

log "Regenerating units from $CURRENT_LINK/scripts/generate-units.sh"
BPT_DEPLOY_ROOT="$BPT_DEPLOY_ROOT" BPT_UNIT_DIR="$STAGING_UNIT_DIR" \
    bash "$CURRENT_LINK/scripts/generate-units.sh" >/dev/null

if ! systemd-analyze verify --user "$STAGING_UNIT_DIR"/*.service 2>&1; then
    log "systemd-analyze verify FAILED on rolled-back units — refusing to swap live units."
    log "Manual recovery: the current/ and previous/ symlinks have ALREADY been swapped back."
    log "You may need to flip them manually, or redeploy a known-good release via deploy.sh."
    rm -rf "$STAGING_UNIT_DIR"
    exit 4
fi

if [ "$RESTART" = "1" ]; then
    log "Stopping bpt-stack.target"
    systemctl --user stop bpt-stack.target || true
fi

log "Copying rolled-back units into $LIVE_UNIT_DIR"
cp -f "$STAGING_UNIT_DIR"/bpt-*.service \
      "$STAGING_UNIT_DIR"/bpt-*.timer \
      "$STAGING_UNIT_DIR"/bpt-*.target \
      "$LIVE_UNIT_DIR/"
rm -rf "$STAGING_UNIT_DIR"

if [ "$LIVE_UNIT_DIR" = "$HOME/.config/systemd/user" ]; then
    log "systemctl --user daemon-reload"
    systemctl --user daemon-reload
fi

if [ "$RESTART" = "1" ]; then
    log "Starting bpt-stack.target"
    systemctl --user start bpt-stack.target
    sleep 3
    systemctl --user is-active bpt-transport.service bpt-refdata.service bpt-md-gateway.service bpt-order-gateway.service bpt-strategy.service bpt-analytics.service bpt-bridge.service || true
fi

log ""
log "=== Rolled back to $prev_target ==="
if [ "$RESTART" = "0" ]; then
    log "Units staged but NOT restarted. When ready:"
    log "  systemctl --user restart bpt-stack.target"
fi
log "Next rollback target: $(readlink "$PREVIOUS_LINK")"

#!/bin/bash
# sync-config.sh — Pull latest main into the checkout that bpt services are
# running against. Intended to be invoked by bpt-config-sync.timer on the
# trading box (or bpt-dev-config-sync.timer on dev).
#
# Semantics:
#   * Config-only refresh. The trading box CONFIG lives in the repo tree
#     ($BPT_ROOT) and is read at runtime by the services. Code changes ship
#     via tagged release tarballs, not via this script.
#   * Safe no-op when there's nothing to pull, or when the working tree is
#     dirty (refuses to run — a human needs to look).
#   * Services do not restart. refdata's internal daily refresh loop picks up
#     new instrument_mapping files on its next tick. Same for other config.
#
# Exits 0 on success (including "nothing to do"), non-zero on any problem
# that blocks the pull — which is what fires the systemd OnFailure= action.
set -euo pipefail

BPT_ROOT="${BPT_ROOT:-/home/jseow/code/bpt-core}"
BRANCH="${BPT_SYNC_BRANCH:-main}"

log() { printf '[sync-config] %s\n' "$*"; }

cd "$BPT_ROOT" || { log "FATAL: BPT_ROOT=$BPT_ROOT does not exist"; exit 1; }

if [ ! -d .git ]; then
    log "FATAL: $BPT_ROOT is not a git checkout"
    exit 1
fi

current_branch=$(git rev-parse --abbrev-ref HEAD)
if [ "$current_branch" != "$BRANCH" ]; then
    log "FATAL: on branch '$current_branch', expected '$BRANCH' — refusing to pull"
    exit 1
fi

if ! git diff --quiet || ! git diff --cached --quiet; then
    log "FATAL: working tree has uncommitted changes — refusing to pull"
    git status --short | head -20 | while read -r line; do log "  $line"; done
    exit 1
fi

before=$(git rev-parse HEAD)
log "fetching origin/$BRANCH (current HEAD $before)"
git fetch --quiet origin "$BRANCH"

if ! git merge-base --is-ancestor "$before" "origin/$BRANCH"; then
    log "FATAL: local HEAD is not an ancestor of origin/$BRANCH — diverged history, refusing to pull"
    exit 1
fi

after=$(git rev-parse "origin/$BRANCH")
if [ "$before" = "$after" ]; then
    log "already up to date at $before"
    exit 0
fi

git merge --ff-only "origin/$BRANCH"
log "updated: $before -> $after"

# Summarise which paths touched by the pull matter to the running services.
changed_config=$(git diff --name-only "$before".."$after" -- config/ || true)
changed_service_cfg=$(git diff --name-only "$before".."$after" -- 'bpt-*/config/' || true)
if [ -n "$changed_config$changed_service_cfg" ]; then
    log "config paths affected:"
    printf '%s\n' "$changed_config" "$changed_service_cfg" | grep -v '^$' | while read -r p; do
        log "  $p"
    done
    log "services will pick up changes on their next internal refresh tick (no restart needed)"
else
    log "no config changes in this pull; binaries do not refresh from git — release tarball handles code"
fi

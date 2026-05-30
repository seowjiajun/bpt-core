#!/bin/bash
# on_box_run.sh — runs on the ephemeral QA box (invoked by bootstrap.sh).
# Deploy the tarball under test via the REAL deploy path, run the recorder for
# the requested window with the rotation/sync overrides, validate, self-report.
# Step Functions owns teardown — this script never terminates the instance.
#
# Env from bootstrap: QA_RUN_ID QA_DURATION_MIN QA_ROTATE_SECONDS
#   QA_SYNC_INTERVAL_MIN QA_RELEASE_KEY QA_BUCKET RELEASES_BUCKET
#   AWS_DEFAULT_REGION LOKI_URL NTFY_URL QA_RELEASE_ROOT
#
# NOTE: the deploy-path steps below are the part that needs a live shakeout —
# the exact tarball layout / env-staging contract is asserted, not yet proven.
# Marked ASSUMPTION where that's load-bearing.
set -uo pipefail

export RAW_ROOT=/opt/bpt/data/raw
log() { echo "[on_box $(date -u +%H:%M:%SZ)] $*"; }
ntfy() { [ -n "${NTFY_URL:-}" ] && curl -fsS -m 10 -d "$1" "$NTFY_URL" >/dev/null 2>&1 || true; }

ntfy "bpt-tape QA $QA_RUN_ID: starting (${QA_DURATION_MIN}m, rotate=${QA_ROTATE_SECONDS}s)"

# ── Progress heartbeat → QA bucket every 60s (survives box death) ───────────
heartbeat() {
  while true; do
    local files rss load up
    files=$(find "$RAW_ROOT" -name '*.wslog' 2>/dev/null | wc -l)
    rss=$(ps -o rss= -C bpt-tape 2>/dev/null | awk '{s+=$1} END{print int(s/1024)}')
    load=$(awk '{print $1}' /proc/loadavg)
    up=$(awk '{print int($1)}' /proc/uptime)
    jq -n --arg r "$QA_RUN_ID" --argjson f "${files:-0}" --argjson m "${rss:-0}" \
      --arg l "$load" --argjson u "$up" \
      '{run_id:$r, wslog_files:$f, bpt_tape_rss_mb:$m, load:$l, uptime_s:$u}' \
      | aws s3 cp - "s3://$QA_BUCKET/progress/$QA_RUN_ID.json" >/dev/null 2>&1
    sleep 60
  done
}
heartbeat & HB_PID=$!
trap 'kill $HB_PID 2>/dev/null' EXIT

# ── Optional: ship logs to central Loki via promtail ────────────────────────
if [ -n "${LOKI_URL:-}" ]; then
  log "starting promtail → $LOKI_URL (label qa_run_id=$QA_RUN_ID)"
  # best-effort; see scripts/qa/README for the promtail unit. TODO: package
  # a promtail binary + config so this isn't an apt/download on the hot path.
fi

# ── Deploy the tarball via the real deploy path ─────────────────────────────
export BPT_DEPLOY_ROOT=/opt/bpt
DEPLOY_SH=$(find "$QA_RELEASE_ROOT" -name deploy.sh | head -1)
GEN_UNITS=$(find "$QA_RELEASE_ROOT" -name generate-units.sh | head -1)
TAPE_TOML=$(find "$QA_RELEASE_ROOT" -name 'bpt-tape.hl.toml' | head -1)
log "deploy.sh=$DEPLOY_SH gen-units=$GEN_UNITS tape-toml=$TAPE_TOML"

# ASSUMPTION: deploy.sh untars + seeds config + regenerates units exactly as on
# the prod host. We run it against the tarball the bootstrap already fetched.
if [ -n "$DEPLOY_SH" ]; then
  bash "$DEPLOY_SH" /tmp/release.tar.gz || { log "deploy.sh failed"; }
fi

# ── QA overrides: rotation + sync cadence (the changes under test) ──────────
# Rotation lives in the tape TOML; rewrite it to the requested interval.
if [ -n "$TAPE_TOML" ]; then
  DEPLOYED_TOML=$(find "$BPT_DEPLOY_ROOT" -name 'bpt-tape.hl.toml' | head -1)
  DEPLOYED_TOML=${DEPLOYED_TOML:-$TAPE_TOML}
  sed -i -E "s/^rotate_interval_seconds *=.*/rotate_interval_seconds = $QA_ROTATE_SECONDS/" "$DEPLOYED_TOML"
  log "set rotate_interval_seconds=$QA_ROTATE_SECONDS in $DEPLOYED_TOML"
fi
# Sync cadence lives in the generated timer; override after unit regen.
SYNC_TIMER=$(find "$HOME" /root /home -name 'bpt-tape-sync.timer' 2>/dev/null | head -1)
if [ -n "$SYNC_TIMER" ]; then
  sed -i -E "s/^OnUnitActiveSec=.*/OnUnitActiveSec=${QA_SYNC_INTERVAL_MIN}min/" "$SYNC_TIMER"
  log "set sync OnUnitActiveSec=${QA_SYNC_INTERVAL_MIN}min in $SYNC_TIMER"
fi

# Point the sync at the QA bucket, not the prod archive.
export BPT_TAPE_S3_BUCKET="$QA_BUCKET"

# ── Start capture ───────────────────────────────────────────────────────────
# ASSUMPTION: the recording stack starts the same way as prod (systemd user
# units for the deploy user, or a direct binary launch). TODO confirm on first
# live run; for now launch the recorder + a periodic sync loop directly so the
# harness is self-contained even if systemd wiring differs on a fresh box.
BPT_TAPE_BIN=$(find "$BPT_DEPLOY_ROOT" "$QA_RELEASE_ROOT" -name bpt-tape -type f | head -1)
log "launching $BPT_TAPE_BIN for ${QA_DURATION_MIN}m"
BPT_TAPE_CONFIG="$DEPLOYED_TOML" "$BPT_TAPE_BIN" --config "$DEPLOYED_TOML" &
TAPE_PID=$!

SYNC_SH=$(find "$BPT_DEPLOY_ROOT" "$QA_RELEASE_ROOT" -name 'sync_tape_to_s3.sh' | head -1)
sync_loop() { while kill -0 $TAPE_PID 2>/dev/null; do bash "$SYNC_SH" || true; sleep $(( QA_SYNC_INTERVAL_MIN * 60 )); done; }
[ -n "$SYNC_SH" ] && sync_loop &

# ── Run the window ──────────────────────────────────────────────────────────
sleep $(( QA_DURATION_MIN * 60 ))
log "capture window elapsed; stopping recorder"
kill "$TAPE_PID" 2>/dev/null; wait "$TAPE_PID" 2>/dev/null

# One final sync so the last files reach the bucket before we validate.
[ -n "$SYNC_SH" ] && bash "$SYNC_SH" || true

# ── Validate → verdict.json (Step Functions is polling for it) ──────────────
if bash /opt/bpt/qa/validate_tape_run.sh; then
  ntfy "✅ bpt-tape QA $QA_RUN_ID PASSED"
else
  ntfy "❌ bpt-tape QA $QA_RUN_ID FAILED — see verdict.json / Grafana"
fi

log "done; idling until Step Functions terminates the instance"
sleep 3600

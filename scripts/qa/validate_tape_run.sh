#!/bin/bash
# validate_tape_run.sh — the QA gate. Runs on the ephemeral box after the
# capture window, asserts the run was healthy + complete, and writes
# verdict.json to the QA bucket. Step Functions polls for that object; its
# {passed} flips the execution green/red.
#
# Env (exported by on_box_run.sh):
#   QA_RUN_ID QA_BUCKET QA_ROTATE_SECONDS QA_DURATION_MIN
#   RAW_ROOT (default /opt/bpt/data/raw)
set -uo pipefail

RAW_ROOT="${RAW_ROOT:-/opt/bpt/data/raw}"
fails=()
note() { echo "[validate] $*"; }

# ── 1. Rotation: did we get roughly the expected number of wslog files? ─────
rotate_min=$(( QA_ROTATE_SECONDS / 60 )); [ "$rotate_min" -lt 1 ] && rotate_min=1
expected=$(( QA_DURATION_MIN / rotate_min - 1 )); [ "$expected" -lt 1 ] && expected=1
mapfile -t wslogs < <(find "$RAW_ROOT" -type f -name '*.wslog' 2>/dev/null)
nfiles=${#wslogs[@]}
note "wslog files: $nfiles (expected >= $expected at ${rotate_min}m rotation)"
[ "$nfiles" -ge "$expected" ] || fails+=("too few wslog files: $nfiles < $expected")

# ── 2. Integrity: every record header parses, timestamps non-decreasing ─────
# Header (little-endian): u64 recv_ts_ns | u8 type | u32 len | payload[len].
parse_ok=1
for f in "${wslogs[@]}"; do
  python3 - "$f" <<'PY' || parse_ok=0
import struct, sys
f = sys.argv[1]
last = 0
with open(f, "rb") as fh:
    data = fh.read()
off, n = 0, len(data)
while off < n:
    if off + 13 > n:
        sys.exit(f"truncated header at {off} in {f}")
    ts, typ, ln = struct.unpack_from("<QBI", data, off)
    off += 13 + ln
    if off > n:
        sys.exit(f"truncated payload (len={ln}) at {off} in {f}")
    if ts < last:
        sys.exit(f"non-monotonic ts in {f}: {ts} < {last}")
    last = ts
PY
done
[ "$parse_ok" = 1 ] || fails+=("wslog parse/monotonicity check failed")

# ── 3. Off-host: did the sync land objects in the QA bucket? ────────────────
objs=$(aws s3 ls "s3://$QA_BUCKET/raw/" --recursive 2>/dev/null | grep -c '\.wslog' || true)
note "objects synced to s3://$QA_BUCKET/raw/: $objs"
[ "${objs:-0}" -ge 1 ] || fails+=("no wslog objects reached the QA bucket")

# ── 4. The 2026-05-30 check: recorder didn't OOM or reboot ──────────────────
uptime_s=$(awk '{print int($1)}' /proc/uptime)
min_uptime=$(( QA_DURATION_MIN * 60 * 9 / 10 ))
note "uptime ${uptime_s}s (expected >= ${min_uptime}s — proves no reboot)"
[ "$uptime_s" -ge "$min_uptime" ] || fails+=("uptime ${uptime_s}s < ${min_uptime}s — box rebooted")
if journalctl -k --no-pager 2>/dev/null | grep -qiE 'out of memory|killed process'; then
  fails+=("kernel OOM-killer fired during the run")
fi

# ── Verdict ─────────────────────────────────────────────────────────────────
if [ ${#fails[@]} -eq 0 ]; then
  passed=true;  reason="all checks passed: $nfiles files, $objs synced, uptime ${uptime_s}s, no OOM"
else
  passed=false; reason=$(IFS='; '; echo "${fails[*]}")
fi
note "VERDICT passed=$passed reason=$reason"

jq -n --argjson passed "$passed" --arg reason "$reason" \
  --argjson files "$nfiles" --argjson objs "${objs:-0}" --argjson uptime "$uptime_s" \
  '{passed:$passed, reason:$reason, metrics:{wslog_files:$files, synced_objects:$objs, uptime_s:$uptime}}' \
  > /tmp/verdict.json

aws s3 cp /tmp/verdict.json "s3://$QA_BUCKET/verdict/$QA_RUN_ID.json"
[ "$passed" = true ]

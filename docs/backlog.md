# Backlog

Known issues and follow-ups that are not urgent enough to block current work
but should be picked up when the related subsystem is next touched.

## Hyperliquid adapter

### WS reconnect reconciliation gap

**Status:** open, observed on HL testnet 2026-04-16.

**Symptom:** HL gracefully closes the `/ws` connection every ~10 minutes
(`"The WebSocket stream was gracefully closed at both endpoints"`). bpt-order-gateway
reconnects within ~2 s, but any `userFills` events generated on the exchange
during the reconnect window are lost — HL does not replay them after the new
subscription. Fenrir's inventory then diverges from the exchange by however
many fills happened in the gap.

**Measured impact (Apr 16 session, pre partial-fill fix):** ~24 fills leaked
across 4 reconnects over ~45 min of trading, causing fenrir to underreport
inventory by ~0.024 BTC vs the real account.

**Why it's currently tolerable:** with the partial-fill + cancel-race fixes
committed today, the observable divergence in steady-state (no reconnects) is
≤ 1 fill (just the natural race between fenrir's log line and an async poll
of `/info`). The reconnect gap is the dominant remaining source of drift.

**Fix options, roughly in order of increasing scope:**

1. **Process `isSnapshot=true` fills on subscribe, dedupe by `oid`.** HL sends
   a recent-fills snapshot on subscribe. If we dedupe by the `(oid, fill_time)`
   pair against an "already seen" set, we can replay missed fills without
   double-counting the ones we already processed. Minimal new state. Simplest
   path forward.
2. **Periodic reconciliation against `clearinghouseState`.** Every ~30 s (or
   on reconnect), fetch `/info { type: "clearinghouseState" }` and compare the
   reported position vs fenrir's internal tracker. If they differ, emit a
   synthetic correction event. Wraps the WS stream with a ground-truth
   safety net. More code but catches any drift mechanism, not just WS gaps.
3. **Keep the WS alive.** HL's ~10-min grace-close cadence may be avoidable by
   a different ping pattern or reconnection strategy — worth checking once the
   account clears the scheduleCancel volume gate and we can use HL's official
   keepalive hooks. Orthogonal to 1 + 2.

Recommended first step: option (1). It's a single-file change in the parser
and directly addresses the observed leak.

**Relevant code:**
- `bpt-order-gateway/src/adapter/hyperliquid/hyperliquid_ws_client.cpp` — reconnect loop
- `bpt-order-gateway/src/adapter/hyperliquid/hyperliquid_exec_parser.cpp` — `handle_fills`
- Log tag to watch: `"gracefully closed at both endpoints"` in `bpt-order-gateway.log`

## bpt-tape

Surfaced 2026-05-10 by the disk-full incident below. Read together: the
recorder is functional but not production-ready, and the failure modes are
silent in ways that make us notice 36+ hours late.

### Disk-full incident, 2026-05-09 00:22 UTC → 2026-05-10 05:34 UTC

**Symptom:** `mkdir("/opt/bpt/data/raw/hyperliquid/2026-05-09")` returned
`ENOSPC` and the writer thread silently looped on retry. Process stayed
"healthy" (WS connected, BBO decoder running, journal streaming) but no wslog
file was open. ~29 hours of HL tape lost. Detected by accident from a stale
local mtime in chat.

**Root cause:** the data EBS (`/dev/nvme1n1`, 30 GB) hit 100% used. May 8
captured 19 GB in 16 hours after a build was deployed at 08:21 UTC — the
post-deploy data rate is ~6× the pre-deploy rate (~3 GB/day before, ~19 GB/day
after). Sync to S3 was working fine but uses `rclone copy`, never deletes, so
old days accumulated until the volume filled.

**Recovery:** verified May 4–8 wslogs were byte-exact on S3, deleted them
locally, the writer's retry loop succeeded on the next mkdir, recording resumed.

### Silent failure: rotation/open errors are swallowed

`bpt-tape` writer retries on `mkdir`/`open` failure with no log line, no
metric, no crash. ENOSPC, EACCES, and EROFS all look identical from outside
the binary: process alive, decoder running, no wslog file. Fix: treat
rotation-open failure as `log_fatal` + abort. With `Restart=always` the
journal records the failure and any monitoring picks it up. Pre-create the
next day's directory at HH:00 (well before the HH:22 rotation) so the failure
mode shifts left and is visible while the previous file is still open.

### Missing local-disk lifecycle

`scripts/sync_tape_to_s3.sh` is `rclone copy` (intentionally not `move`, see
the script comment). Nothing else deletes local files. A separate cleanup
step is needed: after each successful sync, remove local wslogs older than N
days that have a matching `(size, mtime)` on S3. Without this, ENOSPC is
inevitable on any host that records longer than `disk_size / daily_rate`.

### Heartbeat unit not installed on prod

`deploy/env/prod-recorder.env.example` references
`bpt-recording-heartbeat.timer (5-min freshness check → Healthchecks.io)`.
The unit ships in the deploy bundle but was never installed on the Tokyo
recorder host. Without it nothing pages when capture stops. Install it, point
it at a Healthchecks.io check, page on miss.

### No metrics, no dashboard

The bpt-tape config already declares `[metrics] host=127.0.0.1 port=9110` but
nothing scrapes it and nothing visualizes it. Counters needed (some may exist
already, others to add):

- `bpt_tape_last_wslog_write_unix_seconds` (gauge) — single most important;
  alone would have caught this incident
- `bpt_tape_messages_decoded_total{stream}` and `..._persisted_total{stream}`
  — divergence between these is the silent-writer signal
- `bpt_tape_wslog_rotations_total` and `..._rotation_failures_total`
- `bpt_tape_ws_reconnects_total{stream}`
- `bpt_tape_open_fds`
- `bpt_tape_disk_free_bytes{mount="/opt/bpt/data"}`

Grafana dashboard with one row per category, alert rules on
`time_since_last_write > 5min`, `decoded - persisted > 5min`,
`disk_free < 20%`, `ws_reconnects[5m] > 3`.

### Logs only on the host

`/opt/bpt/bpt-tape/logs/bpt-tape.log` rotates by size (~10 MB), keeps a
handful of files, then drops. Anything older than the rotation window can't
be queried after the fact. Ship `journalctl -u bpt-tape` to a central store
(Loki + Promtail is cheapest, CloudWatch Logs is laziest). Keep ≥30 days.

### Terraform drift: prod doesn't match the modules

The repo has `infra/terraform/tape-host`, `tape-storage`, `tape-iam`. They
describe an instance with a 500 GB `/opt/bpt/data` EBS, an Elastic IP,
least-privilege `bpt-tape-archiver` (writer) and `bpt-tape-reader` (reader)
IAM users, and a 3-tier S3 lifecycle. Reality on 2026-05-10:

- `/opt/bpt/data` is **30 GB**, not 500 GB
- Hostname is the AWS-default `ip-10-42-1-221`, not a meaningful DNS name
- Operator's laptop rclone uses **`admin` (root) AWS keys**, not
  `bpt-tape-reader`
- S3 lifecycle policy on the live `bpt-tape-archive` bucket — unverified

Pick a forcing function: either run `terraform plan` against current state
and reconcile, or rebuild the host from the modules into a new instance and
cut over. Either way, drift this large means the IaC isn't a source of
truth.

### Hostname / operability

Even with terraform reconciled, `ip-10-42-1-221` should become
`tape-hl-tokyo-01.bpt.internal` (or similar) — `hostnamectl set-hostname` +
EC2 Name tag (already set to `bpt-tape`, partial credit) + Role/Env/Venue
tags + a Route53 internal record so we ssh by name, not IP. Without DNS,
losing the IP from session shell history (which has happened — the Singapore
IP we tried earlier is now somebody else's box) means scrambling.

### systemd --user instead of system service

Current unit lives in `/home/ubuntu/.config/systemd/user/bpt-tape.service`,
parented by `systemd --user` (PID 11701). It works but binds the recorder's
lifecycle to a user session, depends on `loginctl enable-linger`, and the
deploy bundle's `bpt-recording.target` doesn't apply. Move to a system unit
(`/etc/systemd/system/bpt-tape.service`) under a dedicated `bpt` system
user, matching the prod-recorder.env.example assumption.

### Capacity planning open question

May 4–7: ~3 GB/day. May 8 (post-deploy): ~19 GB/day — files grew from
~150 MB/hour to ~2.1 GB/hour. Either the universe expanded, depth changed,
the refdata REST capture started writing meaningfully, or a new field is now
being recorded. Steady-state rate determines volume sizing, S3 cost
projections, and alert thresholds. Diff the two builds' recording paths and
config to identify which.

**Relevant code / paths:**
- `bpt-tape/src/io/tape.cpp` — Tape open/rotate/write paths
- `scripts/sync_tape_to_s3.sh` — sync script (uses `rclone copy`)
- `deploy/env/prod-recorder.env.example` — references uninstalled heartbeat
- `infra/terraform/tape-{host,storage,iam}/` — design that prod has drifted from
- Ops fact: recorder is `i-0f245857ada6a3312` in `ap-northeast-1`,
  EIP `13.193.107.7`, ssh `ubuntu@13.193.107.7` with `~/.ssh/bpt_tape_ed25519`

### MdValidator interprets `max_price_deviation_pct = 0` as "fail every nonzero deviation"

The bpt-tape config sets `max_price_deviation_pct = 0.0` and the inline
comment says `disable validator on the recorder`. The implementation
treats it as a strict threshold instead — at 0% allowed deviation, every
nonzero tick produces a `[W] [MdValidator] BBO price deviation X% > 0.00%`
warning. On the 230-perp HL recorder this generates **hundreds of
warnings per second** in the journal.

Two ways to fix in `bpt-md-gateway/.../md_validator.{h,cpp}`:

1. Sentinel: treat `max_price_deviation_pct <= 0` as "validation disabled"
   and skip the comparison entirely. Matches the operator's intent and
   keeps the existing config working unchanged.
2. New flag `validation_enabled = false` + leave threshold semantics as
   "strict". More explicit but breaks the "<=0 means disabled" inference
   most readers will form.

(1) is recommended. Until either lands, the recorder bleeds CPU on Quill
serialization for warnings nobody will read, and the journal eats disk
every night when systemd rotates.

**Relevant code:**
- `bpt-md-gateway/include/md_gateway/validation/md_validator.h`
- `bpt-tape/config/bpt-tape.hl.toml:42` — config call site

### bpt-tape has zero unit tests

`find bpt-tape -path "*test*"` returns nothing. The recorder is now a
production service responsible for capturing data we can't recover —
the absence of tests is a P0 deficiency now that we're treating the
output as authoritative.

Three tests would cover the highest-risk paths:

- `test_loader.cpp` — TOML round-trip, missing-required-field errors,
  recording_universe_venues filter. Catches "config silently
  misinterpreted" regressions which are the worst class of recorder bug.
- `test_refdata_poller.cpp` — schedule logic (next_due math), error
  recovery (one bad endpoint doesn't take down siblings), stop()
  responsiveness while a poll is in-flight.
- `test_tape_rotation.cpp` — date-rollover at UTC midnight (the
  exact failure mode from 2026-05-09); ENOSPC simulation via a fake fopen
  that returns `nullptr` once, asserting the new metrics hook fires and
  the bool return is false; also death-test the abort path in the
  recording adapter.

Add `cc_test` target to `bpt-tape/BUILD` mirroring the bpt-common one.

### Missing operational metrics: disk-free + WS reconnect counter

The grafanalib dashboard at `monitoring/dashboards/bpt_tape.dashboard.py`
references `node_filesystem_avail_bytes{mountpoint="/opt/bpt/data"}`,
but the recorder host doesn't run prometheus-node-exporter — those
panels stay blank. Two cheap fixes:

1. Self-emit `bpt_tape_disk_free_bytes{mount="/opt/bpt/data"}` from
   TapeMetrics — one `statvfs()` call every 30s, gauge updated in a
   small helper thread. No external dep, no SG change.
2. Expose `bpt_tape_ws_reconnects_total{venue}`. The data already lives
   in `ConnState::reconnect_count` inside the on_connect/on_disconnect
   closures — surface it via the metrics struct so connection flapping
   is visible on the dashboard rather than buried in journal lines.

Both fit in TapeMetrics with no API change to Tape. Adding a
`DiskUsageRollupAlertRule` would close out the disk side of the alert
matrix once the gauge exists.

### wslog file format has no version stamp

`Tape`'s `SESSION_START` marker writes a JSON payload `{"pid":N,"exchange":...,"ws":...}`
but no format-version field. If the binary record header
(`uint64 ts | uint8 type | uint32 length | payload`) ever changes —
e.g., adding a venue tag, switching to varint length — old wslogs
become unreadable with no programmatic way to detect the schism.

Add `"format_version": 1` to the SESSION_START payload. Then bump on
any record-format change. The replay-side parser checks the version on
file open and refuses to decode formats it doesn't understand, instead
of silently producing garbage. ~15 min of code, ~30 years of forward
compatibility for the archive we're now treating as authoritative.

**Relevant code:**
- `bpt-tape/src/main.cpp` — SESSION_START payload construction
- `bpt-common/include/bpt_common/recorder/wslog_format.h` — RecordType enum
  (also: the unused `CHECKPOINT` value here — write it or remove it)

### bpt-tape cosmetic cleanups (small, batch as one PR)

Low-priority but worth picking up next time the file is open:

- `bpt-tape/src/main.cpp:12` — header comment claims refdata REST
  capture is "a planned extension" but it's been shipped (commit
  `02e7ab7`). Drop the future-tense paragraph.
- Two ad-hoc string-case helpers (`lowercase_venue` in main.cpp,
  `to_upper` in refdata_poller.cpp) — same shape, different files.
  Pull into `bpt-common/util/strings.h`.
- `RecordType::CHECKPOINT` enum value (`tape.h:46`) has
  no producer in the codebase. Either implement the periodic-heartbeat
  write the comment promises, or remove the enum value so readers
  don't think they're missing something.

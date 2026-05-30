# tape-qa — ephemeral QA harness for bpt-tape

On-demand, throwaway environment to test recorder / sync / **deploy-path** changes
against real venue data **without touching the live Tokyo recorder or the
`bpt-tape-archive` data**. Cattle, not pets: provision → run → validate →
destroy. Born out of the 2026-05-30 reboot→OOM incident, where a deploy-path bug
reached the only capture host with no staging gate.

## Why pure AWS (not GitHub Actions)

A GHA job is billed for full wall-clock; an 8h soak ≈ 480 runner-minutes, and the
free tier is 2000/mo (~4 runs). Step Functions `Wait` states bill ~nothing, so an
8h run is a handful of state transitions (fractions of a cent). Teardown is a
guaranteed state (every state `Catch`es to `Terminate`) instead of relying on the
box to self-terminate. The execution graph in the console is the live monitor.

## Shape

```
SSM Automation document  (the parameterized console "form" — the click-UI)
   │  states:StartExecution
   ▼
Step Functions state machine   (statemachine.asl.json.tftpl)
   Provision t3.micro (params as tags) → WaitForBoot → PollVerdict(loop, timeout)
   → Evaluate → Terminate (GUARANTEED via Catch) → Notify(SNS) → Succeed/Fail
   ▼
Ephemeral EC2 (launch template)
   IMDS tags → pull release tarball from bpt-releases → deploy.sh + generate-units.sh
   → bpt-tape for `duration` → validate → verdict.json + progress.json to QA bucket
   → promtail → central Loki (live logs, filter by qa-run-id)
```

## Design choices

- **Params via instance tags** (launch template sets `InstanceMetadataTags=enabled`)
  so the box reads `duration / rotate_seconds / sync_interval / run_id` from IMDS —
  no base64 user_data juggling inside the ASL.
- **QA bucket is persistent** with per-run prefixes + a ~7d lifecycle expiry, not
  create/delete-per-run (no bucket-name churn; data still ages out = "teardown").
- **Same deploy path as prod**: the box runs the real `deploy.sh` +
  `generate-units.sh` on a released tarball — that's what makes it catch
  deploy-path bugs (the OOM bug lived in `generate-units.sh`).
- **Same instance type as prod (t3.micro)** — deliberately; the OOM was
  instance-size-specific. A bigger QA box would hide it.
- **Notifications, two channels by who detects the outcome**: the box curls
  ntfy.sh on normal pass/fail; the SF `Notify`→SNS→email is the safety net for
  what the box can't report (launch error, timeout, dead box).

## Files

| File | Status |
|------|--------|
| `statemachine.asl.json.tftpl` | ✅ written — the orchestration |
| `main.tf` | ⬜ QA bucket (+lifecycle), SNS topic, launch template, `aws_sfn_state_machine`, `aws_ssm_document` |
| `iam.tf` | ⬜ SF execution role (run/terminate by tag, passrole, s3 get verdict, sns), EC2 instance role (s3 RW qa bucket + read releases, ssm), SSM automation role (StartExecution) |
| `variables.tf` / `outputs.tf` | ✅ written |
| `iam.tf` | ✅ written — 3 roles (qa_box / sfn / ssm_automation); terminate tag-gated |
| reaper (2 layers) | ✅ written — layer 1: box `shutdown -h +TTL` + LT `instance_initiated_shutdown_behavior=terminate`; layer 2: `reaper.tf` daily Lambda sweep (`scripts/qa/reaper_lambda.py`) terminating `bpt_qa=true` older than 10h |
| `scripts/qa/on_box_run.sh` | ✅ written (pulled from QA bucket) — deploy → run → validate → verdict + ntfy. Deploy steps flagged ASSUMPTION, need a live shakeout |
| `scripts/qa/validate_tape_run.sh` | ✅ written — the gate assertions (files / parse+monotonic / synced / no-OOM-reboot) |

## Validation gate (what makes it a gate, not a demo)

`validate_tape_run.sh` asserts and writes `verdict.json {passed, reason, metrics}`:
- ≥N wslog files rotated at the configured interval; each parses clean (walk
  `RecordHeader`s); timestamps monotonic
- sync landed ≥M objects under the run's QA-bucket prefix
- **recorder did not OOM/reboot** — `uptime`, `journalctl -k | grep -i oom`, load
  (the check that would have caught 2026-05-30)
- converter runs on one captured file; row/parity sanity

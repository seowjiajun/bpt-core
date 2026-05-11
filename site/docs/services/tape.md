# bpt-tape

**The recording rig.** A dedicated binary that imports `bpt-md-gateway`'s adapter
library and substitutes recording subclasses overriding `on_frame()` to tee raw
WS bytes to disk. The trading-stack `bpt-md-gateway` and `bpt-refdata` binaries
are unchanged — recording lives entirely in this process.

> Renamed from `bpt-md-recorder` on 2026-05-03 to reflect the planned scope
> expansion to refdata REST snapshot capture (instrument list, fee schedules,
> listing events). Tape already supports REST bodies on the design side; the
> per-adapter recording subclass on the refdata side is the remaining work.

## Why a separate binary

The trading stack must never carry recording overhead. The recording host
doesn't need Aeron, doesn't need the MediaDriver, doesn't need other services.
Each role gets a process shape that fits its job:

| Concern | Trading host | Recording host |
|---|---|---|
| Aeron MediaDriver | yes | **no** |
| Per-instrument validation | yes | **disabled** (recorder captures the raw, valid or not) |
| Risk gates | yes | **n/a** (no orders) |
| Memory budget | tight | tiny (~50 MB resident) |
| CPU pinning | yes | optional |
| Disk write rate | low | **dominant** |

`bpt-tape` is purpose-built for that second column.

## Architecture

```
venue WS  →  HyperliquidMdAdapter (mdgw)  ─┐
                                           │ overridden in
                                           ▼
                       RecordingHyperliquidMdAdapter (tape)
                                           │
                  ┌────────────────────────┴──────────────────┐
                  │                                            │
                  ▼                                            ▼
       Tape::write_frame()                  [parent's on_frame() — parses
       → /opt/bpt/data/raw/<venue>/         + would publish, but the publisher
         <UTC-date>/<venue>-HHMMSS.wslog    is NoopMdPublisher, so the SBE
                                            chain compiles down to dead code
                                            the optimiser drops]
```

The recording subclass calls `Tape::write_frame(recv_ns, payload)` *before*
the parent's `on_frame()` — preserves the existing parse pipeline (so latency
metrics still work and any parser drift is visible) but adds a disk tap.

## File format

Append-only binary log. Every record:

```
[recv_ts_ns u64][record_type u8][length u32][payload[length] bytes]
```

`record_type`:

| Value | Meaning |
|---|---|
| 0 | `WS_FRAME` — raw venue payload |
| 1 | `SESSION_START` — recorder process started; payload = config snapshot |
| 2 | `SESSION_STOP` — recorder process stopping cleanly |
| 3 | `CHECKPOINT` — periodic heartbeat |
| 4 | `WS_DISCONNECT` — connection lost |
| 5 | `WS_RECONNECT` — connection re-established |

Output layout on disk:

```
/opt/bpt/data/raw/{venue}/YYYY-MM-DD/{venue}-HHMMSS.wslog
```

One file per recorder rotation interval (default 1 hour). `recv_ts_ns` reflects
the recv time at the recording host — being colocated near the venue matching
engine means these timestamps are authentic to what a co-located trader would
see (see [Tokyo placement](#tokyo)).

## Storage backbone

```mermaid
flowchart LR
    subgraph tokyo["Tokyo VPS"]
        TAPE[bpt-tape] -->|write| HOT[/opt/bpt/data/raw/<br/>30 GB hot tier]
        HOT -->|daily 00:30 UTC| ROTATE[bpt-recording-rotate.timer<br/>wslog → Parquet<br/>+ zstd compress raw]
        HOT -->|hourly| SYNC[bpt-tape-sync.timer<br/>rclone push]
        ROTATE -->|writes| PARQUET[/opt/bpt/data/backtest-cache/]
        PARQUET -->|hourly| SYNC
    end

    SYNC -->|rclone copy| S3STD[S3 Standard<br/>0–30 days]
    S3STD -->|lifecycle| S3IA[S3 Standard-IA<br/>30–365 days<br/>~$12.50/TB/mo]
    S3IA -->|lifecycle| S3DA[S3 Glacier Deep Archive<br/>365+ days<br/>~$1/TB/mo]

    BACKTESTER[Backtester host<br/>laptop or VPS] -.->|on demand pull| S3STD
    BACKTESTER -.->|on demand pull| S3IA
```

## Tokyo

The recording VPS lives in `ap-northeast-1` (Tokyo) for one specific reason: HL's
matching engine sits in Tokyo. Recording from Singapore or US East would inject
50–150 ms of network jitter into every "tick arrived at" timestamp, which corrupts
any latency analysis a backtest can do later. Colocating the tape with the venue
makes the `recv_ts_ns` field authentic to what a future colocated trader would
observe.

Same-region S3 transfers are also free, so the tape host pays $0 to push data
to the same-region archive bucket.

## Cost shape (current testing tier)

| Resource | Sizing | Cost |
|---|---|---|
| EC2 t3.micro 24/7 | Free Tier eligible | $0/mo (then $9.79/mo after 12mo) |
| Root EBS 30 GB gp3 | OS + logs | $2.40/mo |
| Data EBS 30 GB gp3 | hot tier (~2-3 days HL) | $2.40/mo |
| Elastic IP attached | stable address | $0/mo |
| S3 Standard | empty bucket initially | ~$0/mo |
| **Total** | | **~$5/mo during free tier** |

Production sizing is `t3.medium` + 500 GB EBS for ~$72/mo — single-line bumps in
`infra/terraform/tape-host/terraform.tfvars` when graduating from testing.

## Universe selection

Driven by the canonical `instrument_mapping.{venue}.json` file (the same file
`bpt-refdata` reads on a trading host). The recorder loads the JSON at boot,
walks it per venue, applies a declarative filter, and subscribes to every survivor:

```toml
[universe_filter]
inst_types     = ["PERP"]      # only perps
exclude_bases  = ["BTC"]       # too tight a spread for AS to clear fees
default_depth  = 5             # depth-5 ladder
```

Adding/removing recording symbols is an edit to the mapping JSON (which
`bpt-ops` fetchers maintain), not this TOML. Hot-reload tick is on the backlog.

## Why no Aeron

`bpt::app::run` accepts a `RunOptions{connect_aeron=false}` opt-out
([commit c46b390](https://github.com/bishanparktrading/bpt-core/commit/c46b390))
that `bpt-tape` uses. No MediaDriver, no `/dev/shm/aeron-*` directory, no
transport service. The recording host runs zero Aeron infrastructure — venue
WS frames go straight from kernel sockets to disk via `Tape`.

This collapses the recording host's process tree to **one** `systemd` unit:
`bpt-tape.service`. Plus the `bpt-recording-rotate.timer` and
`bpt-tape-sync.timer` that fire daily and hourly respectively.

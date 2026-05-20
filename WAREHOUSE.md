# bpt-canon Warehouse — Path (c) shape

**Status:** design only, build not started. Captures the cheapest legitimate
shape for an OKX historical-data warehouse on top of the canon ingesters
already shipped in `bpt-canon/`. Pick this up via [[project_canon_warehouse]]
in the auto-memory.

Path (c) = **GitHub Actions on a cron + the existing C++ binaries + S3**.
Compared to path (b) (EC2 + EventBridge + Terraform): same warehouse output,
$8/mo cheaper, 1 day less build effort, no infra to manage. The trade is
that it doesn't look like a "real" production ingest pipeline — but you can
swap the GitHub Actions trigger for an EC2 setup later without touching any
of the downstream pieces.

---

## Top-level flow

```
            ┌──────────────────────────────────────────────────────┐
            │                                                      │
            │              OKX  historical-data API                │
            │   (POST priapi/v5/broker/public/trade-data/...)      │
            │                                                      │
            └──────────────────────┬───────────────────────────────┘
                                   │   raw zips / tar.gz
                                   ▼
   ┌────────────────────────────────────────────────────────────────┐
   │  GitHub Actions runner  (ubuntu-latest, free, cron 01:00 UTC) │
   │                                                                │
   │   pulls latest bpt-canon-* binaries from s3://bpt-bin/         │
   │   runs orchestrator.py over yesterday's data, one symbol at a  │
   │   time:                                                        │
   │      fetch → ingest → merge → parquet → push → manifest        │
   │                                                                │
   └──────────────────────┬─────────────────────┬──────────────────┘
                          │                     │
                          ▼                     ▼
            ┌──────────────────────┐  ┌──────────────────────┐
            │  s3://bpt-bin/       │  │  s3://bpt-tape-      │
            │   (binaries, built   │  │     archive/         │
            │   by a 2nd GHA job   │  │   (the warehouse)    │
            │   on every push to   │  │                      │
            │   main)              │  │                      │
            └──────────────────────┘  └──────────┬───────────┘
                                                  │
                                                  │ reads
                                                  ▼
                         ┌──────────────────────────────────────────┐
                         │       Your laptop (consumer)             │
                         │                                          │
                         │   ┌─────────────────┐  ┌──────────────┐ │
                         │   │ bpt-backtester  │  │   DuckDB     │ │
                         │   │ --canon s3://...│  │   on Parquet │ │
                         │   └─────────────────┘  └──────────────┘ │
                         │   (replay backtest)    (ad-hoc research)│
                         └──────────────────────────────────────────┘
```

---

## Producer detail (what runs in GitHub Actions)

```
.github/workflows/
│
├── build-binaries.yml         ◄ on: push to main
│   ┌──────────────────────────────────────────────────────────┐
│   │  bazel build //bpt-canon/...  (ingesters + merger +     │
│   │                                 to-parquet)              │
│   │  aws s3 cp bazel-bin/bpt-canon/bpt-canon-* \             │
│   │     s3://bpt-bin/v${GIT_SHA}/                            │
│   │  aws s3 cp ... s3://bpt-bin/latest/                      │
│   └──────────────────────────────────────────────────────────┘
│
└── daily-ingest.yml           ◄ on: schedule (cron 0 1 * * *)
    ┌──────────────────────────────────────────────────────────┐
    │  steps:                                                  │
    │   1. aws s3 cp s3://bpt-bin/latest/ ./bin/ --recursive   │
    │   2. chmod +x ./bin/bpt-canon-*                          │
    │   3. python orchestrator.py \                            │
    │        --date=$(date -u -d yesterday +%F) \              │
    │        --symbols BTC-USDT-SWAP                           │
    └──────────────────────────────────────────────────────────┘
```

`orchestrator.py` per (venue, symbol, date) — every step is idempotent by
design (rerunning the same `(venue, symbol, date)` produces byte-identical
outputs; safe to retry on partial failures):

```
   ─── 1. fetch ───────────────────────────────────────────────────────
   POST /priapi/v5/broker/public/trade-data/download-link
        ↓ (returns CDN URLs for trades.zip + L2.tar.gz)
   record source URL etag in manifest
   curl + unzip → ./scratch/trades.csv + ./scratch/l2.data
   ASSERT (DQ-bronze): file size ±1% of OKX's reported sizeMB
        ↓
   ─── 2. ingest (silver layer) ───────────────────────────────────────
   ./bin/bpt-canon-ingest-okx-trades  ── trades.canon
   ./bin/bpt-canon-ingest-okx-l2      ── l2.canon
   ./bin/bpt-canon-merge              ── merged.canon
   ASSERT (DQ-silver): canon last_ts − first_ts > 23 hours
                       (catches truncation: full day, not partial)
        ↓
   ─── 3. transform to parquet (gold layer) ───────────────────────────
   ./bin/bpt-canon-to-parquet         ── trades.parquet, books.parquet
   duckdb derivations:
     candles_1m.parquet, candles_5m.parquet, candles_1h.parquet,
     candles_1d.parquet, funding.parquet
   ASSERT (DQ-gold): row counts within ±50% of trailing 7-day median
                     (catches "wire blew up", silent zero-data ingest)
        ↓
   ─── 4. publish atomically ──────────────────────────────────────────
   for each output file:
     sha = sha256(file)
     aws s3 cp file s3://bpt-tape-archive/<hive-partitioned-path>/file
     sqlite UPSERT manifest: (venue, symbol, date, kind, path, sha256,
                              bytes, source_etag, code_sha, produced_at)
   # _SUCCESS marker is written LAST, only if every DQ assert passed.
   # Consumers check for it before reading the partition.
   aws s3 cp /dev/null s3://bpt-tape-archive/canon/venue=okx/.../_SUCCESS
   aws s3 cp /dev/null s3://bpt-tape-archive/parquet/venue=okx/.../_SUCCESS
   aws s3 cp manifest.sqlite s3://bpt-tape-archive/manifest.sqlite
        ↓
   rm -rf ./scratch
```

---

## S3 bucket layout

Reuses the existing `bpt-tape-archive` bucket in `ap-northeast-1` from
`[[project_tape_storage]]`. Two new prefixes; lifecycle policy moves things
cold automatically.

Paths use **Hive-style partitioning** (`venue=…/symbol=…/date=…`) so DuckDB
and any future Spark/Iceberg consumer can prune partitions automatically
without configuration. Each partition directory ends with a `_SUCCESS`
marker file that's written **last**, after all DQ asserts pass —
consumers check for the marker before reading.

```
s3://bpt-tape-archive/
│
├── raw/                                        ◄ Deep Archive from upload
│   └── venue=okx/symbol=BTC-USDT-SWAP/date=2026-04-30/
│       ├── trades.zip
│       ├── L2orderbook-400lv.tar.gz
│       └── _SUCCESS
│
├── canon/                                      ◄ Standard 30d → Deep Archive 180d
│   └── venue=okx/symbol=BTC-USDT-SWAP/date=2026-04-30/
│       ├── trades.canon
│       ├── l2.canon
│       ├── merged.canon                        ◄ what backtester reads
│       └── _SUCCESS
│
├── parquet/                                    ◄ Standard 30d → Glacier IA 180d
│   └── venue=okx/symbol=BTC-USDT-SWAP/date=2026-04-30/
│       ├── trades.parquet
│       ├── books.parquet
│       ├── candles_1m.parquet
│       ├── candles_5m.parquet
│       ├── candles_1h.parquet
│       ├── candles_1d.parquet
│       ├── funding.parquet
│       └── _SUCCESS
│
├── manifest.sqlite                             ◄ Standard, ~1 MB, single file
│   (table: files(venue, symbol, date, kind, path,
│                 sha256, bytes, source_etag,
│                 code_sha, produced_at))
│
└── tape/                                       ◄ existing bpt-tape captures
    └── ...
```

---

## Consumer surfaces (your laptop)

### Backtesting (uses canon)

```
$ bazel-bin/bpt-backtester/bpt-backtester \
    --strategy-config bpt-strategy/config/avellaneda_stoikov.backtest-okx.toml \
    --instrument-mapping config/instruments/instrument_mapping.okx.json \
    --canon s3://bpt-tape-archive/canon/venue=okx/symbol=BTC-USDT-SWAP/date=2026-04-30/merged.canon \
    --output-dir results/
```

`bpt-backtester` grows a small S3 cache (downloads canon to
`/var/cache/bpt-canon/{sha}.canon` on first read, reuses thereafter). Before
fetching, it checks for `_SUCCESS` in the partition prefix — refuses to
read partitions that didn't complete cleanly.

### Research / EDA (uses Parquet, Hive partitions auto-discovered)

```
duckdb> INSTALL httpfs; LOAD httpfs;
duckdb> SET s3_region='ap-northeast-1';
duckdb> -- Hive-partitioned read: venue/symbol/date become free columns.
        -- DuckDB prunes partitions based on the WHERE clause.
duckdb> SELECT venue, symbol, date,
               max(high) - min(low) AS daily_range_usd
        FROM read_parquet(
             's3://bpt-tape-archive/parquet/**/candles_1d.parquet',
             hive_partitioning = true
        )
        WHERE venue='okx' AND symbol='BTC-USDT-SWAP'
              AND date BETWEEN '2026-04-01' AND '2026-04-30'
        GROUP BY venue, symbol, date
        ORDER BY date;
```

DuckDB downloads only the column ranges + partition slices it needs. A full
year of 1m candles is ~20 MB across the wire; partition pruning by date
cuts that to single-MB for narrow queries.

---

## Costs (1 symbol × 1 year, ap-northeast-1)

| Component | Storage | Monthly cost |
|---|---|---|
| Raw archives (Deep Archive) | ~150 GB | $0.15 |
| Canon hot (30d, Standard) | 135 GB | $3.10 |
| Canon cold (335d, Deep Archive) | 1.5 TB | $1.50 |
| Parquet hot (30d, Standard) | ~40 GB | $0.92 |
| Parquet cold (335d, Glacier IA) | ~450 GB | $0.45 |
| Lifecycle transitions | — | $0.10 |
| Binary storage (`s3://bpt-bin/`) | ~500 MB | $0.01 |
| Manifest | ~10 MB | $0 |
| GitHub Actions (300 min/mo, well under free tier) | — | $0.00 |
| Data transfer (S3 → laptop, ~5 GB/mo of EDA reads) | — | $0.60 |
| **Total** | **~2 TB** | **~$6.80/mo** |

For 5 symbols: ~$25/mo. For top-20 OKX universe: ~$100/mo.

---

## Five data-engineering principles baked into this design

These are the practices a senior data eng would insist on even at solo
scale, because they cost almost nothing and pay back the first time
something silently breaks. Reflected throughout the sections above:

1. **Hive-style partitioning.** `venue=okx/symbol=BTC-USDT-SWAP/date=2026-04-30/`
   instead of `okx/2026-04-30/btc-usdt-swap-*`. DuckDB/Spark/Iceberg all do
   automatic partition pruning when the path encodes the columns. Zero
   added complexity vs flat paths, future-proofs the migration to Iceberg
   if scale ever demands it.

2. **`_SUCCESS` marker per partition.** Empty file written *last*, only
   after all DQ asserts pass. Consumers refuse to read a partition
   without one. Eliminates the dominant data-eng failure mode: ingest
   crashes mid-write, downstream reads half-data and produces silently
   wrong PnL.

3. **Manifest rows carry `sha256` + `source_etag` + `code_sha` + `produced_at`.**
   Catches silent corruption (sha mismatch), detects "OKX republished
   yesterday's file" (source_etag changed), enables "what code wrote this?"
   forensics (code_sha), and tracks freshness (produced_at).

4. **Idempotent ingest by design.** Re-running `(venue, symbol, date)`
   must produce byte-identical outputs. Every step pins its inputs: OKX
   file etag, C++ binary SHA, mapping JSON SHA. Lets us safely re-run on
   any failure without worrying about partial-state contamination.

5. **One DQ assert per layer.** Bronze: file size matches OKX's
   reported sizeMB. Silver: canon spans >23 hours. Gold: row counts
   within ±50% of trailing 7-day median. Three lines of code each,
   catches ~80% of "warehouse is silently broken" failures.

These are the practices borrowed from how a real data team at a quant
shop would build this. The full pro stack (Airflow + Iceberg + dbt +
DataHub + Great Expectations) is the right shape once you're past 20
symbols or have 3+ engineers — at solo+single-venue scale it's a 50×
cost multiplier for benefits you can't yet use. Path (c) above is
"professionally-shaped path-c" — same shipping cost as the naive
version, with the five practices above embedded.

When to graduate from this to the full pro stack:
- 1 person, 1-5 symbols → this design is correct
- 1 person, 20+ symbols → add Apache Iceberg (PyIceberg writer, DuckDB
  reader). Same S3 bytes, ACID + schema evolution + time travel.
- Small team, 3+ people → add Prefect or Dagster (orchestrator with UI),
  DataHub (catalog), dbt (SQL transforms with lineage). Free OSS, runs
  locally or on GitHub Actions.
- Production money → Airflow on MWAA, Datadog observability, on-call
  rotation, multi-env staging.

---

## Build order (what to actually do)

| # | Task | Effort |
|---|---|---|
| 1 | Move `/tmp/bpt-canon-test/*` → `/opt/bpt/data/canon/venue=okx/symbol=…/date=…/` (Hive layout, stop losing canon at reboot) | 30 min |
| 2 | Write `bpt-canon-to-parquet` (C++ binary, reads canon → emits Parquet via Arrow C++) | half day |
| 3 | Write `orchestrator.py` (the per-day script with the 5 data-eng principles wired in: idempotency, sha tracking, DQ asserts, `_SUCCESS` markers, Hive paths) | half day |
| 4 | `.github/workflows/build-binaries.yml` + IAM user for the GH Actions runner | 1 hour |
| 5 | `.github/workflows/daily-ingest.yml` | 1 hour |
| 6 | S3 bucket lifecycle policy (one-time, AWS CLI or terraform) | 1 hour |
| 7 | Smoke-test end-to-end with one symbol, one day | 1 hour |

**Total: ~2 focused days.**

---

## Why path (c), the trade with (b)

```
          Path (c) — GitHub Actions             Path (b) — EC2 + EventBridge
       ┌──────────────────────────────┐      ┌──────────────────────────────┐
       │ + free compute (free tier)   │      │ + production-grade ops shape │
       │ + zero infra to provision    │      │ + interview-resume bullet    │
       │ + working warehouse in 2 days│      │ - $8/mo for the compute      │
       │ - "not how a real shop does  │      │ - 1 extra day of terraform/  │
       │   ingest in prod"            │      │   IAM/EventBridge plumbing   │
       └──────────────────────────────┘      └──────────────────────────────┘

                           BOTH produce the SAME warehouse.

   Migration (c) → (b) when you want it: swap daily-ingest.yml for a
   terraform module, point cron at EventBridge instead of GitHub. The
   orchestrator script + the C++ binaries + the S3 layout + the
   consumer side are all unchanged.
```

The argument for starting with (c): *you don't know yet what the right
ingest cadence, error handling, or symbol coverage actually looks like.*
Iterating on a yaml file is faster than iterating on terraform + IAM.
Lock in the operational ergonomics on the cheap version, then migrate
the runner when the shape stops changing.

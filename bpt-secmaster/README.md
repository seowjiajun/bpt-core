# bpt-secmaster

Central source of truth for **what instruments exist and what exchanges
they trade on.** Everything downstream — strategies, ogw, mdgw,
backtester, dashboard — references `instrument_id` as the universal
join key; secmaster is where that id is allocated and where every
field describing the instrument originates.

See `SECMASTER.d2` (and rendered `SECMASTER.svg`) at the repo root for
the architecture diagram.

## Status

| Phase | What | Status |
|---|---|---|
| 1 | Package scaffolding | ✓ |
| 2 | Schema (CREATE TABLEs, SCD-2, seed exchanges) | ✓ |
| 3 | Refresh Lambda + venue ingesters (Python) | ✓ |
| 4 | Terraform infra (RDS, Lambda, EventBridge, ECR, IAM, Secrets) | ✓ |
| 5 | `pgweb` browse UI (trading host systemd unit) | ✓ |
| 6 | Production architecture: Path A (sidecar + S3 + trading-host pull) | ✓ |
| 7 | Delete legacy in-tree `instrument_mapping.<venue>.json` files | — |

**Phase 6 decision**: chose **Path A (sidecar)** over direct C++
`PostgresSecmasterSource`. The Lambda renders the legacy JSON shape and
uploads to S3; trading host pulls via systemd timer. No C++ changes to
bpt-refdata. See SECMASTER.d2 for the full architecture; see commit
message of `secmaster: productionize Path A` for the rationale.

## Why it exists

Today: per-venue instrument data lives in `instrument_mapping.<venue>.json`
in git. That works at 1-host, 1-strategy scale but has known costs:
no audit history, no atomic rollback, no schema enforcement, no
queryable cross-venue view, awkward concurrent updates from a daily
refresher.

Secmaster moves the data into a proper RDBMS (AWS RDS PostgreSQL,
Free Tier 12mo then ~15 USD/mo) with:

- **Internal `instrument_id` allocation** — opaque uint32, never
  reused. Same id across venues for the same economic instrument
  (BTC perp on OKX and Hyperliquid both resolve to one id).
- **SCD-2 history on every field** — answers "what was BTC-USDT-SWAP's
  tick size on 2026-04-12?" via SQL, not git archaeology. Required
  for backtest reproducibility once multi-strategy lands.
- **Symbology xref as a separate table** — bridges CCXT/FIGI/ISIN/
  venue-native namespaces without schema churn each time a new vendor
  appears.
- **Daily auto-refresh from venue REST APIs** — runs as an AWS Lambda
  decoupled from the trading host (so trading restarts don't skip
  ingest, and ingest failures don't touch trading).

## Architecture (one paragraph)

EventBridge fires Lambda daily 03:00 UTC. Lambda pulls each venue's
public instruments endpoint, derives the canonical symbol, and upserts
to RDS with SCD-2 timestamps. Trading host's `bpt-refdata` opens a
libpqxx connection at startup, hydrates an in-memory cache, writes a
last-known-good snapshot file, and republishes via Aeron to all
consumers (strategies, ogw, mdgw, etc.) — exactly as today, only the
upstream changes. If RDS is unreachable at startup, bpt-refdata
falls back to the snapshot file with a loud warning. Browse the
catalog via `pgweb` on port 8081 (SSH-tunnel from your laptop).

See `SECMASTER.d2` for the diagram.

## Layout

```
bpt-secmaster/
├── README.md                    (this file)
├── schema/
│   └── 001_initial.sql          ◄ phase 2 (current)
├── lambda/refresh/              ◄ phase 3
├── admin/                       ◄ phase 3
│   ├── seed_from_json.py
│   └── fix_instrument.py
└── infra/terraform/             ◄ phase 4
```

## Schema overview

Six tables (full DDL in `schema/001_initial.sql`):

| Table | Purpose | SCD-2? |
|---|---|---|
| `meta` | Schema version + key-value config | no |
| `exchange` | One row per venue; id matches `ExchangeId.h` enum | no |
| `instrument` | Economic instrument; opaque internal_id; dedup by canonical_symbol | yes |
| `listing` | M:N instrument × exchange; tick, lot, native symbol, fees | yes |
| `symbology` | M:N external identifiers (ccxt, figi, isin, venue_native, …) | yes |
| `event` | Audit log of structural changes (renames, tick changes, …) | no (events are immutable) |
| `ingest_run` | One row per refresher Lambda run, per venue | no |

## Canonical symbol grammar (CCXT-extended)

| Class | Grammar | Example |
|---|---|---|
| Spot | `<BASE>/<QUOTE>` | `BTC/USDT` |
| Linear perp | `<BASE>/<QUOTE>:PERPETUAL` | `BTC/USDT:PERPETUAL` |
| Inverse perp | `<BASE>/<QUOTE>:PERPETUAL.INVERSE` | `BTC/USD:PERPETUAL.INVERSE` |
| Dated future | `<BASE>/<QUOTE>:<YYYYMMDD>` | `BTC/USDT:20251226` |
| Option | `<BASE>/<QUOTE>:<YYYYMMDD>-<STRIKE>-<C\|P>` | `BTC/USDT:20251226-90000-C` |
| Index | `<BASE>/<QUOTE>.INDEX` | `BTC/USD.INDEX` |

The canonical symbol is **venue-agnostic** and is the dedup key across
listings. BTC perp on OKX, HL, and Binance all map to
`BTC/USDT:PERPETUAL` (the USDT-quoted linear flavor) and share one
internal_id.

## SCD-2 convention

A row is "current" if `valid_to IS NULL`. Updates close the old row
(set `valid_to = now()`) and insert a new row (set
`valid_from = now()`). Partial unique indexes enforce that exactly
one row per natural key (canonical_symbol for instrument;
instrument_id+exchange_id for listing) is current at any time.

As-of query template:
```sql
SELECT *
FROM instrument
WHERE canonical_symbol = 'BTC/USDT:PERPETUAL'
  AND valid_from <= '2026-04-12'::timestamptz
  AND (valid_to IS NULL OR valid_to > '2026-04-12'::timestamptz);
```

## Bootstrapping

See `infra/terraform/README.md` for the full first-time bootstrap. Short
version:

```bash
cd bpt-secmaster/infra/terraform
cp terraform.tfvars.example terraform.tfvars && $EDITOR terraform.tfvars
terraform init && terraform apply

DSN=$(terraform output -raw fetch_dsn_command | bash)
psql "$DSN" -f ../../schema/001_initial.sql

cd ..
./deploy.sh   # build + push Lambda image, point Lambda at it

aws lambda invoke --function-name bpt-secmaster-refresh /tmp/out.json
cat /tmp/out.json
```

## Access (pgweb on the trading host)

See `systemd/README.md` for the full install. Short version:

```bash
# on the trading host (one-time):
sudo bpt-secmaster/systemd/install-pgweb.sh

# on your laptop:
ssh -L 8081:localhost:8081 trading-host
open http://localhost:8081
```

The pgweb unit runs as a sandboxed systemd service bound to 127.0.0.1
only; DSN delivered via systemd-creds (host-bound), pgweb itself in
`--readonly` mode.

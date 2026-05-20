-- bpt-secmaster · initial schema (v1)
-- Target: AWS RDS PostgreSQL 16.x (db.t4g.micro Free Tier)
--
-- Six tables:
--   meta          schema versioning, key-value config
--   exchange      ~5-10 rows, ABI-stable id matches messages/ExchangeId.h
--   instrument    economic instrument, opaque uint32 internal_id, SCD-2
--   listing       M:N instrument × exchange with venue-specific params, SCD-2
--   symbology     M:N external identifiers (ccxt, figi, isin, venue_native)
--   event         audit log of structural changes (renames, tick changes, …)
--   ingest_run    log of refresher Lambda runs for ops + audit
--
-- SCD-2 convention: only one row per (canonical_symbol) for instrument and
-- (instrument_id, exchange_id) for listing has valid_to IS NULL — the current
-- row. Updates close the old row (valid_to = now()) and insert a new row
-- (valid_from = now()). As-of queries: WHERE valid_from <= ts AND
-- (valid_to IS NULL OR valid_to > ts).

BEGIN;

-- ─────────────────────────────── meta ────────────────────────────────
-- Schema versioning lets bpt-refdata detect "secmaster is newer than I
-- know about" and degrade gracefully.
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

INSERT INTO meta (key, value) VALUES
    ('schema_version', '1'),
    ('created_at',     now()::TEXT)
ON CONFLICT (key) DO NOTHING;

-- ────────────────────────────── exchange ─────────────────────────────
-- One row per venue. The id is ABI-stable wire format — matches the
-- enum in messages/schema/bpt-protocol.xml (ExchangeId). Reserve
-- 5-15 for future venues so adapters don't need recompile when added.
CREATE TABLE IF NOT EXISTS exchange (
    id              SMALLINT PRIMARY KEY CHECK (id > 0 AND id < 256),

    -- Short lowercase code. Used in S3 paths, log lines, config keys.
    -- e.g. 'okx', 'hl', 'binance', 'deribit', 'bybit'.
    code            TEXT NOT NULL UNIQUE CHECK (code = LOWER(code)),

    -- UI-only display name. e.g. 'OKX', 'Hyperliquid', 'Binance'.
    display_name    TEXT NOT NULL,

    -- ISO 10383 MIC code if the venue has one (crypto venues mostly don't).
    mic             TEXT,
    region          TEXT,

    -- Comma-separated for now: 'spot,linear-perp,inverse-perp,option,future'.
    -- Split into a junction table later if we need to query by class.
    asset_classes   TEXT NOT NULL,

    -- Default public maker/taker fee in bps. Per-account tiers live elsewhere
    -- (in credentials store, not secmaster).
    base_maker_bps  NUMERIC(6, 3),
    base_taker_bps  NUMERIC(6, 3),

    api_docs_url    TEXT,

    status          TEXT NOT NULL DEFAULT 'live'
                    CHECK (status IN ('live', 'sunsetted')),

    created_at      TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- ───────────────────────────── instrument ────────────────────────────
-- The economic instrument. internal_id is the universal join key
-- referenced by every downstream system (strategy, ogw, mdgw, fills,
-- positions, …). Opaque integer, never carries meaning, never reused.
--
-- Dedup key: canonical_symbol (CCXT-extended grammar).
--   spot           BTC/USDT
--   linear perp    BTC/USDT:PERPETUAL
--   inverse perp   BTC/USD:PERPETUAL.INVERSE
--   dated future   BTC/USDT:20251226
--   option         BTC/USDT:20251226-90000-C
--   index          BTC/USD.INDEX
--
-- Two listings on different venues with the same canonical_symbol map
-- to the SAME internal_id. That's how cross-venue arb gets its join
-- key for free.
CREATE TABLE IF NOT EXISTS instrument (
    -- BIGSERIAL but range-constrained to uint32 (4B values, more than
    -- enough; 4 bytes on the wire vs 8 for uint64).
    id              BIGSERIAL PRIMARY KEY
                    CHECK (id > 0 AND id < 4294967296),

    canonical_symbol TEXT NOT NULL,

    class           TEXT NOT NULL
                    CHECK (class IN ('spot', 'linear-perp', 'inverse-perp',
                                     'future', 'option', 'index')),

    -- Currency triple. settle differs from quote for inverse perps
    -- (BTC/USD:PERPETUAL.INVERSE settles in BTC).
    base_ccy        TEXT NOT NULL,
    quote_ccy       TEXT NOT NULL,
    settle_ccy      TEXT NOT NULL,

    -- Contract face value in base currency units. 1.0 for spot.
    multiplier      NUMERIC(20, 10) NOT NULL DEFAULT 1.0,

    -- Derivatives-only.
    expiry          DATE,
    strike          NUMERIC(20, 8),
    putcall         CHAR(1) CHECK (putcall IN ('C', 'P')),

    -- Cross-venue arb grouping. Default: instrument is its own family.
    -- Same family_id for instruments that should be netted/hedged together
    -- (e.g. OKX vs Binance linear BTC perp).
    family_id       BIGINT,

    -- SCD-2 history columns.
    valid_from      TIMESTAMPTZ NOT NULL DEFAULT now(),
    valid_to        TIMESTAMPTZ,

    -- Audit.
    changed_by      TEXT NOT NULL DEFAULT 'system',
    change_source   TEXT,  -- 'refresh.lambda', 'admin.fix_instrument', 'seed', ...
    change_reason   TEXT
);

-- Exactly one current row per canonical_symbol.
CREATE UNIQUE INDEX IF NOT EXISTS instrument_canonical_current_unique
    ON instrument (canonical_symbol) WHERE valid_to IS NULL;

-- For SCD-2 as-of queries.
CREATE INDEX IF NOT EXISTS instrument_canonical_history_idx
    ON instrument (canonical_symbol, valid_from, valid_to);

CREATE INDEX IF NOT EXISTS instrument_family_idx ON instrument (family_id);

-- ───────────────────────────── listing ───────────────────────────────
-- Per-venue listing of an instrument. M:N instrument × exchange.
-- Holds the venue-specific stuff: native symbol, tick, lot, fees.
CREATE TABLE IF NOT EXISTS listing (
    id              BIGSERIAL PRIMARY KEY,
    instrument_id   BIGINT NOT NULL REFERENCES instrument (id),
    exchange_id     SMALLINT NOT NULL REFERENCES exchange (id),

    -- Venue's API string verbatim. e.g. 'BTC-USDT-SWAP' (OKX),
    -- 'BTC' (HL), 'BTCUSDT' (Binance), 'BTC-29DEC25-90000-C' (Deribit).
    venue_native_symbol TEXT NOT NULL,

    -- Venue-specific trading parameters.
    tick_size       NUMERIC(20, 10) NOT NULL,
    lot_size        NUMERIC(20, 10) NOT NULL,
    min_qty         NUMERIC(20, 10),
    min_notional    NUMERIC(20, 4),

    -- Venue-side fee override (defaults to exchange.base_*_bps).
    maker_bps       NUMERIC(6, 3),
    taker_bps       NUMERIC(6, 3),

    -- Listing lifecycle.
    listed_at       DATE,
    delisted_at     DATE,
    status          TEXT NOT NULL DEFAULT 'live'
                    CHECK (status IN ('live', 'suspended', 'delisted')),

    -- SCD-2 + audit.
    valid_from      TIMESTAMPTZ NOT NULL DEFAULT now(),
    valid_to        TIMESTAMPTZ,
    changed_by      TEXT NOT NULL DEFAULT 'system',
    change_source   TEXT,
    change_reason   TEXT
);

-- Exactly one current listing per (instrument, exchange) pair.
CREATE UNIQUE INDEX IF NOT EXISTS listing_current_unique
    ON listing (instrument_id, exchange_id) WHERE valid_to IS NULL;

CREATE INDEX IF NOT EXISTS listing_history_idx
    ON listing (instrument_id, exchange_id, valid_from, valid_to);

CREATE INDEX IF NOT EXISTS listing_exchange_idx ON listing (exchange_id);

-- For reverse lookup: "what instrument has venue_native='BTC-USDT-SWAP' on OKX?"
CREATE INDEX IF NOT EXISTS listing_venue_native_idx
    ON listing (exchange_id, venue_native_symbol) WHERE valid_to IS NULL;

-- ──────────────────────────── symbology ──────────────────────────────
-- M:N table for cross-vendor identifiers. Each row maps one external ID
-- to one internal instrument. New vendor namespace = new rows, no
-- schema migration.
--
-- Bootstrapping vendors: 'ccxt', 'figi', 'isin', 'cusip', 'venue_native'
-- (mirrors listing.venue_native_symbol but keyed by vendor for uniform
-- lookup), 'opra' (options), 'on_chain_contract' (defi tokens).
CREATE TABLE IF NOT EXISTS symbology (
    instrument_id   BIGINT NOT NULL REFERENCES instrument (id),
    vendor          TEXT NOT NULL,
    value           TEXT NOT NULL,
    valid_from      TIMESTAMPTZ NOT NULL DEFAULT now(),
    valid_to        TIMESTAMPTZ,
    PRIMARY KEY (instrument_id, vendor, value)
);

-- Reverse lookup: "what instrument has CCXT symbol X?"
CREATE INDEX IF NOT EXISTS symbology_vendor_value_idx
    ON symbology (vendor, value) WHERE valid_to IS NULL;

-- ─────────────────────────────── event ───────────────────────────────
-- Audit log of structural changes. Distinct from SCD-2 history: SCD-2
-- tracks row state, events name the *kind* of change. Used by the UI's
-- "events" tab and by the daily ingest report.
CREATE TABLE IF NOT EXISTS event (
    id              BIGSERIAL PRIMARY KEY,
    instrument_id   BIGINT REFERENCES instrument (id),
    listing_id      BIGINT REFERENCES listing (id),

    event_type      TEXT NOT NULL CHECK (event_type IN (
        'listed', 'delisted', 'suspended', 'resumed',
        'rename', 'tick_size_change', 'lot_size_change',
        'multiplier_change', 'fee_change', 'other'
    )),

    event_at        TIMESTAMPTZ NOT NULL,
    old_value       JSONB,
    new_value       JSONB,
    source          TEXT NOT NULL,  -- 'refresh.lambda', 'admin', 'venue_notice', ...
    description     TEXT
);

CREATE INDEX IF NOT EXISTS event_instrument_idx ON event (instrument_id);
CREATE INDEX IF NOT EXISTS event_at_idx ON event (event_at);
CREATE INDEX IF NOT EXISTS event_type_idx ON event (event_type, event_at);

-- ──────────────────────────── ingest_run ─────────────────────────────
-- One row per Lambda invocation, per venue. Operator looks at this to
-- answer "did refresh run today? what happened?"
CREATE TABLE IF NOT EXISTS ingest_run (
    id              BIGSERIAL PRIMARY KEY,
    source          TEXT NOT NULL,
    started_at      TIMESTAMPTZ NOT NULL,
    finished_at     TIMESTAMPTZ,
    status          TEXT NOT NULL CHECK (status IN ('running', 'ok', 'partial', 'failed')),
    rows_added      INTEGER NOT NULL DEFAULT 0,
    rows_modified   INTEGER NOT NULL DEFAULT 0,
    rows_unchanged  INTEGER NOT NULL DEFAULT 0,
    rows_removed    INTEGER NOT NULL DEFAULT 0,
    error_count     INTEGER NOT NULL DEFAULT 0,
    notes           TEXT
);

CREATE INDEX IF NOT EXISTS ingest_run_started_idx ON ingest_run (started_at DESC);
CREATE INDEX IF NOT EXISTS ingest_run_source_idx ON ingest_run (source, started_at DESC);

-- ──────────────────────── seed exchange rows ─────────────────────────
-- The id values MUST match messages/ExchangeId.h enum. Adding a new
-- venue here requires bumping the SBE schema too.
INSERT INTO exchange (id, code, display_name, asset_classes, status) VALUES
    (1, 'binance',     'Binance',     'spot,linear-perp,inverse-perp', 'live'),
    (2, 'okx',         'OKX',         'spot,linear-perp,inverse-perp,option', 'live'),
    (3, 'hl',          'Hyperliquid', 'linear-perp,spot', 'live'),
    (4, 'deribit',     'Deribit',     'linear-perp,future,option', 'live')
ON CONFLICT (id) DO NOTHING;

COMMIT;

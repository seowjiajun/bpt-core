"""
Render the current secmaster state into the legacy instrument_mapping.json
shape that bpt-refdata's InstrumentMappingLoader reads.

The Lambda calls this at the end of every successful refresh — produces a
JSON blob in memory + uploads to S3. Trading hosts pull from S3.

Mirrors the standalone admin/render_instrument_mapping.py CLI (which
operators run locally for ad-hoc rendering). The two share the same
schema-to-JSON mapping logic; kept in sync by convention, not import.
"""

from __future__ import annotations

import json
from collections import defaultdict
from typing import Any

import psycopg
from psycopg.rows import dict_row


# secmaster class → legacy `type` field. None = skip (e.g. indexes).
CLASS_TO_LEGACY_TYPE = {
    "spot": "SPOT",
    "linear-perp": "PERP",
    "inverse-perp": "PERP",
    "future": "FUTURE",
    "option": "OPTION",
    "index": None,
}

# Binance reuses `symbol` across spot + perp surfaces (BTCUSDT means both).
# The legacy loader disambiguates by suffixing `_SPOT` on the forward-key.
BINANCE_EXCHANGE_ID = 1


def render_mapping_json(conn: psycopg.Connection) -> str:
    """Build the instrument_mapping JSON string from the current secmaster state."""
    rows = _fetch_rows(conn)
    forward, reverse = _build_maps(rows)
    payload = {"forward": forward, "reverse": reverse}
    # sort_keys=True + indent=2 = stable diffs across renders. Same input
    # → same bytes → S3 ETag stable → trading-host pulls skip via
    # ETag-conditional GET.
    return json.dumps(payload, indent=2, sort_keys=True)


def _fetch_rows(conn: psycopg.Connection) -> list[dict[str, Any]]:
    sql = """
        SELECT i.id              AS canonical_id,
               i.canonical_symbol,
               i.class           AS sm_class,
               i.base_ccy,
               i.quote_ccy,
               l.exchange_id,
               l.venue_native_symbol
        FROM instrument i
        JOIN listing l ON l.instrument_id = i.id
        WHERE i.valid_to IS NULL
          AND l.valid_to IS NULL
          AND l.status = 'live'
        ORDER BY i.id, l.exchange_id
    """
    with conn.cursor(row_factory=dict_row) as cur:
        cur.execute(sql)
        return list(cur.fetchall())


def _build_maps(rows: list[dict[str, Any]]) -> tuple[dict, dict]:
    forward: dict[str, int] = {}
    reverse: dict[str, dict] = defaultdict(
        lambda: {"base": "", "quote": "", "type": "", "exchanges": {}}
    )

    for r in rows:
        legacy_type = CLASS_TO_LEGACY_TYPE.get(r["sm_class"])
        if legacy_type is None:
            continue

        cid = r["canonical_id"]
        eid = r["exchange_id"]
        sym = r["venue_native_symbol"]

        key = f"{eid}_{sym}"
        if eid == BINANCE_EXCHANGE_ID and legacy_type == "SPOT":
            key = f"{eid}_{sym}_SPOT"

        forward[key] = cid

        entry = reverse[str(cid)]
        entry["base"] = r["base_ccy"]
        entry["quote"] = r["quote_ccy"]
        entry["type"] = legacy_type
        entry["exchanges"][str(eid)] = sym

    return forward, dict(reverse)

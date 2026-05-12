from __future__ import annotations

import json
from pathlib import Path

import polars as pl

_INSTRUMENT_TYPE = {
    "SPOT": "SPOT",
    "MARGIN": "SPOT",
    "SWAP": "PERP",
    "FUTURES": "FUTURES",
    "PERP": "PERP",
    "OPTION": "OPTION",
}


def load_refdata(path: Path) -> pl.DataFrame:
    """Read the unified refdata snapshot at /opt/bpt/data/instrument_mapping.json.

    Returns a frame with one row per (exchange, symbol) pair. Columns are the
    minimum metadata downstream features need; everything else stays in the
    raw JSON for ad-hoc lookups.
    """
    raw = json.loads(Path(path).read_text())
    instruments = raw.get("instruments", [])
    rows = [
        {
            "instrument_id": inst.get("id"),
            "exchange": inst.get("exchange"),
            "symbol": inst.get("symbol"),
            "inst_type": _INSTRUMENT_TYPE.get(inst.get("inst_type"), inst.get("inst_type")),
            "base_ccy": inst.get("base_ccy"),
            "quote_ccy": inst.get("quote_ccy"),
            "tick_size": float(inst.get("tick_size") or 0.0),
            "lot_size": float(inst.get("lot_size") or 0.0),
            "min_size": float(inst.get("min_size") or 0.0),
        }
        for inst in instruments
        if inst.get("symbol") and inst.get("exchange")
    ]
    return pl.DataFrame(rows)


def load_md_samples(path: Path | None) -> pl.LazyFrame | None:
    """Optional: lazy-scan a parquet file with MD samples (one row per BBO tick).

    Expected columns: instrument_id, ts_ns, best_bid, best_ask, bid_size, ask_size.
    Returns None when path is unset — features that need MD will be skipped.
    """
    if path is None:
        return None
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"md samples not found: {p}")
    return pl.scan_parquet(p)


def load_fills(path: Path | None) -> pl.LazyFrame | None:
    """Optional: lazy-scan a parquet file with historical fills (markout source).

    Expected columns: instrument_id, ts_ns, side, price, qty, fee_paid, mid_at_fill,
    mid_30s_after, mid_60s_after.
    Returns None when path is unset — markout features will be skipped.
    """
    if path is None:
        return None
    p = Path(path)
    if not p.exists():
        raise FileNotFoundError(f"fills not found: {p}")
    return pl.scan_parquet(p)

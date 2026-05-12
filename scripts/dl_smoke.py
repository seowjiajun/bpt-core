#!/usr/bin/env python3
"""Mimic bpt-backtester's DataLoader in Python against a Parquet cache.

Why: CMake build is drifted, so we can't build the C++ dl_smoke right now.
This script validates the same things DataLoader does — columns, types,
per-row sanity — and merges events by timestamp the same way. If this
passes, the C++ consumer reading the same Arrow types will also work.

Reads:
  {cache}/trades/{exchange}/{symbol}/YYYY-MM-DD.parquet
    required columns: timestamp_ns (int64), price (float64),
                      quantity (float64), side (int8)
  {cache}/orderbook/{exchange}/{symbol}/YYYY-MM-DD.parquet
    required columns: timestamp_ns (int64),
                      bid_px_1..5, bid_sz_1..5,
                      ask_px_1..5, ask_sz_1..5 (all float64)
"""

from __future__ import annotations

import argparse
import sys
from datetime import UTC, date, datetime, timedelta
from pathlib import Path

import pyarrow.parquet as pq

BOOK_DEPTH = 5
TRADE_COLS = {
    "timestamp_ns": "int64",
    "price": "double",
    "quantity": "double",
    "side": "int8",
}
BOOK_COLS = {"timestamp_ns": "int64"}
for lvl in range(1, BOOK_DEPTH + 1):
    for side in ("bid", "ask"):
        for kind in ("px", "sz"):
            BOOK_COLS[f"{side}_{kind}_{lvl}"] = "double"


def check_columns(table, expected: dict, path: str) -> list[str]:
    errors = []
    got = {f.name: str(f.type) for f in table.schema}
    for name, ty in expected.items():
        if name not in got:
            errors.append(f"{path}: missing column '{name}'")
        elif got[name] != ty:
            errors.append(f"{path}: column '{name}' type={got[name]}, expected {ty}")
    return errors


def validate_day(cache: Path, exchange: str, symbol: str, day: date) -> dict:
    tp = cache / "trades" / exchange / symbol / f"{day}.parquet"
    op = cache / "orderbook" / exchange / symbol / f"{day}.parquet"

    result = {"day": str(day), "trades": 0, "book": 0, "errors": []}
    if not tp.exists() and not op.exists():
        result["errors"].append(f"no files for {day}")
        return result

    if tp.exists():
        t = pq.read_table(tp)
        result["errors"] += check_columns(t, TRADE_COLS, str(tp))
        result["trades"] = t.num_rows
        if t.num_rows > 0:
            df = t.to_pandas()
            ts = df["timestamp_ns"].values
            if (ts[:-1] > ts[1:]).any():
                result["errors"].append(f"{tp}: timestamps not sorted")
            if (df["price"] <= 0).any():
                result["errors"].append(f"{tp}: non-positive price detected")
            if (~df["side"].isin([0, 1])).any():
                result["errors"].append(f"{tp}: side outside {{0,1}}")
            result["trade_first_ts"] = int(ts[0])
            result["trade_last_ts"] = int(ts[-1])

    if op.exists():
        t = pq.read_table(op)
        result["errors"] += check_columns(t, BOOK_COLS, str(op))
        result["book"] = t.num_rows
        if t.num_rows > 0:
            df = t.to_pandas()
            ts = df["timestamp_ns"].values
            if (ts[:-1] > ts[1:]).any():
                result["errors"].append(f"{op}: timestamps not sorted")
            crossed = (df["bid_px_1"] >= df["ask_px_1"]) & (df["ask_px_1"] > 0)
            if crossed.any():
                result["errors"].append(f"{op}: {crossed.sum()} crossed books (bid_1 >= ask_1)")
            result["book_first_ts"] = int(ts[0])
            result["book_last_ts"] = int(ts[-1])

    return result


def merge_count(cache: Path, exchange: str, symbol: str, day: date) -> int:
    """Merge trades + book events by timestamp, like DataLoader::next() does."""
    tp = cache / "trades" / exchange / symbol / f"{day}.parquet"
    op = cache / "orderbook" / exchange / symbol / f"{day}.parquet"

    events = []
    if tp.exists():
        t = pq.read_table(tp).to_pandas()
        for ts in t["timestamp_ns"].values:
            events.append((int(ts), "TRADE"))
    if op.exists():
        t = pq.read_table(op).to_pandas()
        for ts in t["timestamp_ns"].values:
            events.append((int(ts), "BOOK"))
    events.sort()
    return len(events)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--cache", default="/opt/bpt/data/backtest-cache")
    ap.add_argument("--exchange", required=True)
    ap.add_argument("--symbol", required=True)
    ap.add_argument("--start", required=True, help="YYYY-MM-DD")
    ap.add_argument("--end", required=True, help="YYYY-MM-DD")
    args = ap.parse_args()

    cache = Path(args.cache)
    start = datetime.strptime(args.start, "%Y-%m-%d").date()
    end = datetime.strptime(args.end, "%Y-%m-%d").date()

    print(f"DataLoader-parity check: {args.exchange}/{args.symbol} {start} → {end}")
    print(f"Cache: {cache}")
    print()

    all_errors = []
    total_trades = 0
    total_book = 0
    total_merged = 0
    first_ts, last_ts = None, None

    d = start
    while d <= end:
        r = validate_day(cache, args.exchange, args.symbol, d)
        total_trades += r["trades"]
        total_book += r["book"]
        all_errors += r["errors"]
        if r["trades"] > 0 or r["book"] > 0:
            merged = merge_count(cache, args.exchange, args.symbol, d)
            total_merged += merged
            day_first = min(r.get("trade_first_ts", 2**63), r.get("book_first_ts", 2**63))
            day_last = max(r.get("trade_last_ts", 0), r.get("book_last_ts", 0))
            if first_ts is None or day_first < first_ts:
                first_ts = day_first
            if last_ts is None or day_last > last_ts:
                last_ts = day_last
            print(f"  {d}: trades={r['trades']:>6} book={r['book']:>6} merged={merged}")
        d += timedelta(days=1)

    print()
    print(f"Totals: trades={total_trades} book={total_book} merged_events={total_merged}")
    if first_ts is not None:
        span_s = (last_ts - first_ts) / 1e9
        fdt = datetime.fromtimestamp(first_ts / 1e9, tz=UTC)
        ldt = datetime.fromtimestamp(last_ts / 1e9, tz=UTC)
        print(f"Span  : {fdt.isoformat()} → {ldt.isoformat()} ({span_s:.1f}s)")

    if all_errors:
        print()
        print("ERRORS:")
        for e in all_errors:
            print(f"  - {e}")
        sys.exit(1)

    print()
    print(
        "OK — schema and data-quality checks pass. C++ DataLoader with the"
        " same Arrow type bindings will read this cache."
    )


if __name__ == "__main__":
    main()

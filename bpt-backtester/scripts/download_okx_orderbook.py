#!/usr/bin/env python3
"""
Download OKX historical L2 order book data and convert to Jormungandr Parquet format.

Source format: newline-delimited JSON inside a .data file within a .tar.gz archive.
Each line: {"instId":..., "action":"snapshot"|"update", "ts":"<ms>",
             "asks":[["price","size","orders"],...], "bids":[...]}

Reconstructs the order book from snapshot + update messages, then downsamples
to one row per SAMPLE_MS interval.

Usage:
    python download_okx_orderbook.py \
        --symbol BTC-USDT-SWAP \
        --start 2025-03-20 \
        --end   2025-03-26 \
        --output /data/jormungandr

Output layout:
    {output}/OKX/{symbol}/orderbook/YYYY-MM-DD.parquet

Parquet schema (matches Jormungandr DataLoader):
    timestamp_ns  int64
    bid_px_1..5   float64   (best bid = level 1)
    bid_sz_1..5   float64
    ask_px_1..5   float64   (best ask = level 1)
    ask_sz_1..5   float64
"""

import argparse
import io
import json
import sys
import tarfile
import time
from datetime import date, timedelta
from pathlib import Path

import pandas as pd
import pyarrow as pa
import pyarrow.parquet as pq
import requests

# ── Constants ──────────────────────────────────────────────────────────────────

CDN_BASE  = "https://static.okx.com/cdn/okx/match/orderbook/L2/400lv/daily"
DEPTH     = 5    # Jormungandr kOrderBookDepth
SAMPLE_MS = 100  # Keep one snapshot per this many milliseconds (0 = keep all)

# ── Helpers ────────────────────────────────────────────────────────────────────

def build_url(symbol: str, day: date) -> str:
    folder   = day.strftime("%Y%m%d")
    filename = f"{symbol}-L2orderbook-400lv-{day.strftime('%Y-%m-%d')}.tar.gz"
    return f"{CDN_BASE}/{folder}/{filename}"


def download_bytes(url: str, retries: int = 3) -> bytes:
    for attempt in range(1, retries + 1):
        try:
            resp = requests.get(url, timeout=120)
            resp.raise_for_status()
            return resp.content
        except requests.HTTPError as e:
            if e.response.status_code == 404:
                raise FileNotFoundError(f"404 — file not found: {url}") from e
            if attempt == retries:
                raise
            print(f"    attempt {attempt} failed ({e}), retrying…")
            time.sleep(2 ** attempt)
    raise RuntimeError("unreachable")


def apply_levels(book: dict, levels: list) -> None:
    """Apply a list of [price, size, ...] levels to a book-side dict. Size '0' = remove."""
    for level in levels:
        px = float(level[0])
        sz = float(level[1])
        if sz == 0.0:
            book.pop(px, None)
        else:
            book[px] = sz


def emit_row(ts_ms: int, bids: dict, asks: dict) -> dict | None:
    """Emit a row from the current book state. Returns None if book is too shallow."""
    sorted_bids = sorted(bids.items(), reverse=True)
    sorted_asks = sorted(asks.items())
    if len(sorted_bids) < DEPTH or len(sorted_asks) < DEPTH:
        return None
    row = {"timestamp_ns": ts_ms * 1_000_000}
    for i in range(DEPTH):
        row[f"bid_px_{i+1}"] = sorted_bids[i][0]
        row[f"bid_sz_{i+1}"] = sorted_bids[i][1]
    for i in range(DEPTH):
        row[f"ask_px_{i+1}"] = sorted_asks[i][0]
        row[f"ask_sz_{i+1}"] = sorted_asks[i][1]
    return row


def parse_data_file(fileobj, sample_ms: int) -> pd.DataFrame:
    """
    Parse an OKX .data file (newline-delimited JSON) into a DataFrame.
    Reconstructs the order book from snapshot + update messages.
    Downsampled to sample_ms intervals.
    """
    rows = []
    bids: dict[float, float] = {}
    asks: dict[float, float] = {}
    last_ts_bucket = -1

    for raw_line in fileobj:
        line = raw_line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue

        action = msg.get("action")
        if action not in ("snapshot", "update"):
            continue

        ts_ms = int(msg["ts"])

        if action == "snapshot":
            bids = {float(l[0]): float(l[1]) for l in msg.get("bids", [])}
            asks = {float(l[0]): float(l[1]) for l in msg.get("asks", [])}
        else:
            apply_levels(bids, msg.get("bids", []))
            apply_levels(asks, msg.get("asks", []))

        if sample_ms > 0:
            bucket = ts_ms // sample_ms
            if bucket == last_ts_bucket:
                continue
            last_ts_bucket = bucket

        row = emit_row(ts_ms, bids, asks)
        if row is not None:
            rows.append(row)

    return pd.DataFrame(rows)


def extract_and_parse(data: bytes, sample_ms: int) -> pd.DataFrame:
    """Extract the .data file from a tar.gz archive and parse it."""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        for member in tar.getmembers():
            if member.isfile():
                f = tar.extractfile(member)
                if f is None:
                    continue
                print(f"    reading {member.name} ({member.size / 1e9:.2f} GB uncompressed)")
                return parse_data_file(io.TextIOWrapper(f, encoding="utf-8"), sample_ms)
    raise RuntimeError("No data file found inside tar.gz")


def write_parquet(df: pd.DataFrame, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    schema = pa.schema([
        ("timestamp_ns", pa.int64()),
        *[(f"bid_px_{i}", pa.float64()) for i in range(1, DEPTH + 1)],
        *[(f"bid_sz_{i}", pa.float64()) for i in range(1, DEPTH + 1)],
        *[(f"ask_px_{i}", pa.float64()) for i in range(1, DEPTH + 1)],
        *[(f"ask_sz_{i}", pa.float64()) for i in range(1, DEPTH + 1)],
    ])
    table = pa.Table.from_pandas(df, schema=schema, preserve_index=False)
    pq.write_table(table, path, compression="snappy")


# ── Main ───────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Download OKX L2 orderbook → Jormungandr Parquet")
    parser.add_argument("--symbol",   default="BTC-USDT-SWAP", help="OKX instrument ID")
    parser.add_argument("--start",    required=True,            help="Start date YYYY-MM-DD (inclusive)")
    parser.add_argument("--end",      required=True,            help="End date   YYYY-MM-DD (inclusive)")
    parser.add_argument("--output",   required=True,            help="Root output directory")
    parser.add_argument("--exchange", default="OKX",            help="Exchange label in output path")
    parser.add_argument("--sample-ms", type=int, default=SAMPLE_MS,
                        help=f"Downsample to one row per N ms (default {SAMPLE_MS}, 0=keep all)")
    args = parser.parse_args()

    sample_ms = args.sample_ms
    start  = date.fromisoformat(args.start)
    end    = date.fromisoformat(args.end)
    outdir = Path(args.output)

    day = start
    while day <= end:
        outpath = outdir / args.exchange / args.symbol / "orderbook" / f"{day}.parquet"

        if outpath.exists():
            print(f"{day}  already exists, skipping")
            day += timedelta(days=1)
            continue

        url = build_url(args.symbol, day)
        print(f"{day}  downloading {url}")

        try:
            raw = download_bytes(url)
        except FileNotFoundError as e:
            print(f"{day}  SKIP — {e}")
            day += timedelta(days=1)
            continue

        df = extract_and_parse(raw, sample_ms)

        if df.empty:
            print(f"{day}  WARNING: no usable rows after filtering")
            day += timedelta(days=1)
            continue

        write_parquet(df, outpath)
        size_mb = outpath.stat().st_size / 1e6
        print(f"{day}  wrote {len(df):,} rows → {outpath}  ({size_mb:.1f} MB)")

        day += timedelta(days=1)

    print("\nDone.")


if __name__ == "__main__":
    main()

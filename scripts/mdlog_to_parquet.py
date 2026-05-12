#!/usr/bin/env python3
"""Convert bpt-tape .mdlog files to bpt-backtester's Parquet layout.

File format written by the recorder (little-endian):
    recv_ts_ns (u64) | length (u32) | SBE frame (length bytes)

SBE message header (8 bytes): blockLength u16 | templateId u16 | schemaId u16 | version u16

Decoded template IDs:
    7  MdMarketData — BBO (bid/ask/qty + timestamp)
    8  MdTrade
    20 MdOrderBook — variable-length with bids/asks groups

Output layout (matches bpt-backtester/src/data/data_loader.cpp):
    {out}/trades/{exchange}/{symbol}/YYYY-MM-DD.parquet
        columns: timestamp_ns i64, price f64, quantity f64, side i8
    {out}/orderbook/{exchange}/{symbol}/YYYY-MM-DD.parquet
        columns: timestamp_ns i64, bid_px_1..5 f64, bid_sz_1..5 f64,
                 ask_px_1..5 f64, ask_sz_1..5 f64

If no MdOrderBook frames are seen, a degenerate 1-level book is synthesized
from MdMarketData BBO so backtester can still open the file (levels 2-5 = 0.0).
"""

from __future__ import annotations

import argparse
import glob
import struct
import sys
from collections import defaultdict
from datetime import UTC, datetime
from pathlib import Path

try:
    import pyarrow as pa
    import pyarrow.parquet as pq
except ImportError:
    print("error: pyarrow not installed; run `pip install pyarrow`", file=sys.stderr)
    sys.exit(1)


SBE_HEADER_FMT = "<HHHH"
SBE_HEADER_SIZE = struct.calcsize(SBE_HEADER_FMT)
RECORD_PREFIX_FMT = "<QI"
RECORD_PREFIX_SIZE = struct.calcsize(RECORD_PREFIX_FMT)

TID_MD_MARKET_DATA = 7
TID_MD_TRADE = 8
TID_MD_ORDERBOOK = 20

BOOK_DEPTH = 5

MD_MARKET_DATA_FMT = "<QQdddd Q".replace(" ", "")
MD_TRADE_FMT = "<QQdd BQ".replace(" ", "")
MD_ORDERBOOK_BLOCK_FMT = "<QQQ"
MD_ORDERBOOK_BLOCK_SIZE = struct.calcsize(MD_ORDERBOOK_BLOCK_FMT)
GROUP_HEADER_FMT = "<HH"
GROUP_HEADER_SIZE = struct.calcsize(GROUP_HEADER_FMT)
BOOK_LEVEL_FMT = "<dd"
BOOK_LEVEL_SIZE = struct.calcsize(BOOK_LEVEL_FMT)


def day_key(ts_ns: int) -> str:
    dt = datetime.fromtimestamp(ts_ns / 1e9, tz=UTC)
    return dt.strftime("%Y-%m-%d")


def parse_mdlog(path: Path):
    """Yield (recv_ts_ns, template_id, body_bytes) tuples for each record."""
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    n = len(data)
    while off + RECORD_PREFIX_SIZE <= n:
        recv_ts_ns, length = struct.unpack_from(RECORD_PREFIX_FMT, data, off)
        off += RECORD_PREFIX_SIZE
        if off + length > n:
            print(f"warn: truncated record at {path}:{off}, stopping", file=sys.stderr)
            return
        frame = data[off : off + length]
        off += length
        if len(frame) < SBE_HEADER_SIZE:
            continue
        _block_len, template_id, _schema_id, _version = struct.unpack_from(SBE_HEADER_FMT, frame, 0)
        yield recv_ts_ns, template_id, frame[SBE_HEADER_SIZE:]


def decode_md_market_data(body: bytes):
    """Returns (timestamp_ns, instrument_id, bid_px, bid_qty, ask_px, ask_qty, seq_num)."""
    return struct.unpack_from(MD_MARKET_DATA_FMT, body, 0)


def decode_md_trade(body: bytes):
    """Returns (timestamp_ns, instrument_id, price, qty, side_u8, seq_num)."""
    return struct.unpack_from(MD_TRADE_FMT, body, 0)


def decode_md_orderbook(body: bytes):
    """Returns (timestamp_ns, instrument_id, seq_num, bids, asks) where each side is
    a list of (price, qty) tuples truncated to BOOK_DEPTH."""
    ts, inst, seq = struct.unpack_from(MD_ORDERBOOK_BLOCK_FMT, body, 0)
    off = MD_ORDERBOOK_BLOCK_SIZE

    def read_group():
        nonlocal off
        if off + GROUP_HEADER_SIZE > len(body):
            return []
        block_len, num_in_group = struct.unpack_from(GROUP_HEADER_FMT, body, off)
        off += GROUP_HEADER_SIZE
        levels = []
        for _ in range(num_in_group):
            if off + block_len > len(body):
                break
            px, qty = struct.unpack_from(BOOK_LEVEL_FMT, body, off)
            levels.append((px, qty))
            off += block_len
        return levels

    bids = read_group()
    asks = read_group()
    return ts, inst, seq, bids, asks


def write_trades_parquet(path: Path, rows: list):
    if not rows:
        return
    rows.sort(key=lambda r: r[0])
    path.parent.mkdir(parents=True, exist_ok=True)
    table = pa.table(
        {
            "timestamp_ns": pa.array([r[0] for r in rows], type=pa.int64()),
            "price": pa.array([r[1] for r in rows], type=pa.float64()),
            "quantity": pa.array([r[2] for r in rows], type=pa.float64()),
            "side": pa.array([r[3] for r in rows], type=pa.int8()),
        }
    )
    pq.write_table(table, path)
    print(f"wrote {path} ({table.num_rows} trades)")


def write_orderbook_parquet(path: Path, rows: list):
    if not rows:
        return
    rows.sort(key=lambda r: r["timestamp_ns"])
    path.parent.mkdir(parents=True, exist_ok=True)
    cols = {
        "timestamp_ns": pa.array([r["timestamp_ns"] for r in rows], type=pa.int64()),
    }
    for lvl in range(1, BOOK_DEPTH + 1):
        cols[f"bid_px_{lvl}"] = pa.array([r["bid_px"][lvl - 1] for r in rows], type=pa.float64())
        cols[f"bid_sz_{lvl}"] = pa.array([r["bid_sz"][lvl - 1] for r in rows], type=pa.float64())
        cols[f"ask_px_{lvl}"] = pa.array([r["ask_px"][lvl - 1] for r in rows], type=pa.float64())
        cols[f"ask_sz_{lvl}"] = pa.array([r["ask_sz"][lvl - 1] for r in rows], type=pa.float64())
    table = pa.table(cols)
    pq.write_table(table, path)
    print(f"wrote {path} ({table.num_rows} book snapshots)")


def convert(
    input_glob: str,
    output_dir: Path,
    exchange: str,
    symbol: str,
    instrument_id: int | None,
    synthesize_book_from_bbo: bool,
):
    trades_by_day = defaultdict(list)
    book_by_day = defaultdict(list)
    bbo_by_day = defaultdict(list)

    frames_seen = {TID_MD_MARKET_DATA: 0, TID_MD_TRADE: 0, TID_MD_ORDERBOOK: 0}
    frames_filtered = 0

    files = sorted(glob.glob(input_glob, recursive=True))
    if not files:
        print(f"error: no files matched {input_glob}", file=sys.stderr)
        sys.exit(1)

    for f in files:
        for _recv_ts_ns, tid, body in parse_mdlog(Path(f)):
            if tid == TID_MD_MARKET_DATA:
                ts, inst, bpx, bqty, apx, aqty, _seq = decode_md_market_data(body)
                if instrument_id is not None and inst != instrument_id:
                    frames_filtered += 1
                    continue
                frames_seen[tid] += 1
                bbo_by_day[day_key(ts)].append((ts, bpx, bqty, apx, aqty))
            elif tid == TID_MD_TRADE:
                ts, inst, px, qty, side, _seq = decode_md_trade(body)
                if instrument_id is not None and inst != instrument_id:
                    frames_filtered += 1
                    continue
                frames_seen[tid] += 1
                trades_by_day[day_key(ts)].append((ts, px, qty, side))
            elif tid == TID_MD_ORDERBOOK:
                ts, inst, _seq, bids, asks = decode_md_orderbook(body)
                if instrument_id is not None and inst != instrument_id:
                    frames_filtered += 1
                    continue
                frames_seen[tid] += 1
                row = {
                    "timestamp_ns": ts,
                    "bid_px": [0.0] * BOOK_DEPTH,
                    "bid_sz": [0.0] * BOOK_DEPTH,
                    "ask_px": [0.0] * BOOK_DEPTH,
                    "ask_sz": [0.0] * BOOK_DEPTH,
                }
                for i, (p, q) in enumerate(bids[:BOOK_DEPTH]):
                    row["bid_px"][i] = p
                    row["bid_sz"][i] = q
                for i, (p, q) in enumerate(asks[:BOOK_DEPTH]):
                    row["ask_px"][i] = p
                    row["ask_sz"][i] = q
                book_by_day[day_key(ts)].append(row)

    print(
        f"frames: bbo={frames_seen[TID_MD_MARKET_DATA]} "
        f"trade={frames_seen[TID_MD_TRADE]} "
        f"book={frames_seen[TID_MD_ORDERBOOK]} "
        f"filtered={frames_filtered}"
    )

    if not book_by_day and synthesize_book_from_bbo and bbo_by_day:
        print("note: no MdOrderBook frames seen — synthesizing 1-level book from BBO")
        for day, bbos in bbo_by_day.items():
            for ts, bpx, bqty, apx, aqty in bbos:
                row = {
                    "timestamp_ns": ts,
                    "bid_px": [bpx, 0.0, 0.0, 0.0, 0.0],
                    "bid_sz": [bqty, 0.0, 0.0, 0.0, 0.0],
                    "ask_px": [apx, 0.0, 0.0, 0.0, 0.0],
                    "ask_sz": [aqty, 0.0, 0.0, 0.0, 0.0],
                }
                book_by_day[day].append(row)

    for day, rows in trades_by_day.items():
        out = output_dir / "trades" / exchange / symbol / f"{day}.parquet"
        write_trades_parquet(out, rows)

    for day, rows in book_by_day.items():
        out = output_dir / "orderbook" / exchange / symbol / f"{day}.parquet"
        write_orderbook_parquet(out, rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--input",
        required=True,
        help="Glob for .mdlog files, e.g. /opt/bpt/data/raw/okx/**/*.mdlog",
    )
    ap.add_argument(
        "--output",
        default="/opt/bpt/data/backtest-cache",
        help="Output Parquet root (bpt-backtester local_cache)",
    )
    ap.add_argument("--exchange", required=True, help="e.g. OKX")
    ap.add_argument("--symbol", required=True, help="e.g. BTC-USDT-SPOT")
    ap.add_argument(
        "--instrument-id",
        type=int,
        default=None,
        help="Only keep frames matching this instrumentId (omit = keep all)",
    )
    ap.add_argument(
        "--no-synthesize-book",
        action="store_true",
        help="If MdOrderBook is absent, do NOT fake it from BBO",
    )
    args = ap.parse_args()

    convert(
        input_glob=args.input,
        output_dir=Path(args.output),
        exchange=args.exchange,
        symbol=args.symbol,
        instrument_id=args.instrument_id,
        synthesize_book_from_bbo=not args.no_synthesize_book,
    )


if __name__ == "__main__":
    main()

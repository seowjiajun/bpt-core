#!/usr/bin/env python3
"""Convert raw OKX WS .wslog files (recorded by bpt-md-gateway) to the Parquet
layout bpt-backtester reads.

File format (little-endian) per record:
    recv_ts_ns u64 | record_type u8 | length u32 | payload bytes (length)

record_type:
    0 = WS_FRAME (raw OKX JSON)
    1 = SESSION_START (config snapshot JSON)
    2 = SESSION_STOP  (exit reason JSON)
    3 = CHECKPOINT    (heartbeat JSON {frames, bytes})

OKX WS shapes handled:
    {"event":"subscribe"|"error", ...}                  — control, skipped
    {"arg":{"channel":"trades", ...}, "data":[ ... ]}   — trade prints
    {"arg":{"channel":"books"|"books5"|"books-l2-tbt", ...},
     "action":"snapshot"|"update", "data":[{"asks":..., "bids":..., "ts":..., "seqId":...}]}

Output:
    {out}/trades/{exchange}/{symbol}/YYYY-MM-DD.parquet
    {out}/orderbook/{exchange}/{symbol}/YYYY-MM-DD.parquet
    {out}/gaps.json — manifest of detected gaps + session bracket health
"""

from __future__ import annotations

import argparse
import glob
import json
import struct
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

try:
    import pyarrow as pa
    import pyarrow.parquet as pq
except ImportError:
    print("error: pyarrow not installed; run `pip install pyarrow`", file=sys.stderr)
    sys.exit(1)

RECORD_HEADER_FMT = "<QBI"
RECORD_HEADER_SIZE = struct.calcsize(RECORD_HEADER_FMT)

REC_WS_FRAME = 0
REC_SESSION_START = 1
REC_SESSION_STOP = 2
REC_CHECKPOINT = 3

BOOK_DEPTH = 5
SIDE_BUY = 0
SIDE_SELL = 1


def parse_records(path: Path):
    """Yield (recv_ts_ns, record_type, payload_bytes) for each record in a .wslog."""
    with open(path, "rb") as f:
        data = f.read()
    off = 0
    n = len(data)
    while off + RECORD_HEADER_SIZE <= n:
        recv_ts_ns, rtype, length = struct.unpack_from(RECORD_HEADER_FMT, data, off)
        off += RECORD_HEADER_SIZE
        if off + length > n:
            print(f"warn: truncated record at {path}:{off}", file=sys.stderr)
            return
        payload = data[off : off + length]
        off += length
        yield recv_ts_ns, rtype, payload


def day_key(ts_ns: int) -> str:
    return datetime.fromtimestamp(ts_ns / 1e9, tz=timezone.utc).strftime("%Y-%m-%d")


def parse_okx_frame(text: str):
    """Parse one OKX WS JSON frame. Yields ('trade'|'book'|'control', ...).
    See parse_frame() for tuple shapes."""
    try:
        msg = json.loads(text)
    except (json.JSONDecodeError, ValueError):
        return
    if not isinstance(msg, dict):
        return
    if "event" in msg:
        return  # subscribe ack / error

    arg = msg.get("arg") or {}
    channel = arg.get("channel") or ""
    inst = arg.get("instId") or ""
    data = msg.get("data") or []

    if channel == "trades" and data:
        for d in data:
            try:
                ts_ns = int(d["ts"]) * 1_000_000
                px = float(d["px"])
                qty = float(d["sz"])
                side = SIDE_BUY if d.get("side") == "buy" else SIDE_SELL
                yield ("trade", inst, ts_ns, px, qty, side)
            except (KeyError, ValueError, TypeError):
                continue
    elif channel.startswith("book") and data:
        for d in data:
            try:
                ts_ns = int(d["ts"]) * 1_000_000
                action = msg.get("action", "update")
                bids = [(float(b[0]), float(b[1])) for b in (d.get("bids") or [])]
                asks = [(float(a[0]), float(a[1])) for a in (d.get("asks") or [])]
                yield ("book", inst, ts_ns, action, bids, asks)
            except (KeyError, ValueError, TypeError, IndexError):
                continue


def parse_hyperliquid_frame(text: str):
    """Parse one Hyperliquid WS JSON frame.

    Frame shapes:
      l2Book:  {"channel":"l2Book","data":{"coin":"BTC","time":<ms>,"levels":[[bids],[asks]]}}
                 levels[0] = bids (best→worst), levels[1] = asks (best→worst)
                 each level = {"px":"...","sz":"...","n":N}
                 HL sends FULL snapshot every update — treat as 'snapshot' action.
      trades:  {"channel":"trades","data":[{"coin":"BTC","side":"B"|"A","px":"...","sz":"...","time":<ms>,...}]}
                 "B" = buy aggressor (taker bought) → side=0 (BUY)
                 "A" = sell aggressor (taker sold) → side=1 (SELL)
      Other channels (activeAssetCtx, pong, subscriptionResponse) ignored.
    """
    try:
        msg = json.loads(text)
    except (json.JSONDecodeError, ValueError):
        return
    if not isinstance(msg, dict):
        return
    channel = msg.get("channel") or ""
    data = msg.get("data")

    if channel == "trades" and isinstance(data, list):
        for d in data:
            try:
                coin = d["coin"]
                ts_ns = int(d["time"]) * 1_000_000
                px = float(d["px"])
                qty = float(d["sz"])
                side = SIDE_BUY if d.get("side") == "B" else SIDE_SELL
                yield ("trade", coin, ts_ns, px, qty, side)
            except (KeyError, ValueError, TypeError):
                continue
    elif channel == "l2Book" and isinstance(data, dict):
        try:
            coin = data["coin"]
            ts_ns = int(data["time"]) * 1_000_000
            levels = data.get("levels") or [[], []]
            bids = [(float(lv["px"]), float(lv["sz"])) for lv in levels[0]] if len(levels) > 0 else []
            asks = [(float(lv["px"]), float(lv["sz"])) for lv in levels[1]] if len(levels) > 1 else []
            yield ("book", coin, ts_ns, "snapshot", bids, asks)
        except (KeyError, ValueError, TypeError):
            return


def parse_frame(text: str, exchange: str):
    if exchange == "OKX":
        yield from parse_okx_frame(text)
    elif exchange in ("HYPERLIQUID", "HL"):
        yield from parse_hyperliquid_frame(text)
    else:
        raise ValueError(f"unknown --exchange {exchange!r} — expected OKX or HYPERLIQUID")


class OrderBookState:
    """Maintains an L2 ladder per instrument; emits top-N snapshots."""
    def __init__(self):
        self.bids: dict[float, float] = {}
        self.asks: dict[float, float] = {}

    def apply(self, action: str, bids, asks):
        if action == "snapshot":
            self.bids = {p: s for p, s in bids if s > 0}
            self.asks = {p: s for p, s in asks if s > 0}
        else:  # update
            for p, s in bids:
                if s == 0:
                    self.bids.pop(p, None)
                else:
                    self.bids[p] = s
            for p, s in asks:
                if s == 0:
                    self.asks.pop(p, None)
                else:
                    self.asks[p] = s

    def top_n(self, n: int):
        bids = sorted(self.bids.items(), key=lambda x: -x[0])[:n]
        asks = sorted(self.asks.items(), key=lambda x: x[0])[:n]
        return bids, asks


def write_trades_parquet(path: Path, rows: list):
    if not rows:
        return
    rows.sort(key=lambda r: r[0])
    path.parent.mkdir(parents=True, exist_ok=True)
    table = pa.table({
        "timestamp_ns": pa.array([r[0] for r in rows], type=pa.int64()),
        "price": pa.array([r[1] for r in rows], type=pa.float64()),
        "quantity": pa.array([r[2] for r in rows], type=pa.float64()),
        "side": pa.array([r[3] for r in rows], type=pa.int8()),
    })
    pq.write_table(table, path)
    print(f"wrote {path} ({table.num_rows} trades)")


def write_book_parquet(path: Path, rows: list):
    if not rows:
        return
    rows.sort(key=lambda r: r["timestamp_ns"])
    path.parent.mkdir(parents=True, exist_ok=True)
    cols = {"timestamp_ns": pa.array([r["timestamp_ns"] for r in rows], type=pa.int64())}
    for lvl in range(1, BOOK_DEPTH + 1):
        cols[f"bid_px_{lvl}"] = pa.array([r["bid_px"][lvl - 1] for r in rows], type=pa.float64())
        cols[f"bid_sz_{lvl}"] = pa.array([r["bid_sz"][lvl - 1] for r in rows], type=pa.float64())
        cols[f"ask_px_{lvl}"] = pa.array([r["ask_px"][lvl - 1] for r in rows], type=pa.float64())
        cols[f"ask_sz_{lvl}"] = pa.array([r["ask_sz"][lvl - 1] for r in rows], type=pa.float64())
    table = pa.table(cols)
    pq.write_table(table, path)
    print(f"wrote {path} ({table.num_rows} book snapshots)")


def detect_gaps(events: list, max_quiet_ns: int):
    """Walk through (recv_ts, record_type, info) tuples in order and emit gap dicts.

    A gap is any of:
      - SESSION_START without a prior SESSION_STOP — previous session ended uncleanly
      - Two records >max_quiet_ns apart with no intervening CHECKPOINT
      - A file ends without a SESSION_STOP record

    `events` is sorted by recv_ts. Returns list[dict].
    """
    gaps = []
    last_clean_close = None
    last_seen_ts = None
    have_open_session = False

    for ts, rtype, info in events:
        if rtype == REC_SESSION_START:
            if have_open_session:
                gaps.append({
                    "reason": "unclean_shutdown",
                    "gap_end_ns": ts,
                    "detail": "SESSION_START with previous session still open",
                    "previous_seen_ts_ns": last_seen_ts,
                })
            elif last_clean_close is not None and ts - last_clean_close > 0:
                gaps.append({
                    "reason": "process_down",
                    "gap_start_ns": last_clean_close,
                    "gap_end_ns": ts,
                    "detail": "process restarted after clean shutdown",
                })
            have_open_session = True
            last_clean_close = None
        elif rtype == REC_SESSION_STOP:
            have_open_session = False
            last_clean_close = ts
        elif rtype == REC_WS_FRAME:
            if last_seen_ts is not None and ts - last_seen_ts > max_quiet_ns:
                gaps.append({
                    "reason": "long_quiet_no_checkpoint",
                    "gap_start_ns": last_seen_ts,
                    "gap_end_ns": ts,
                    "detail": f"no frames or checkpoints for {(ts - last_seen_ts)/1e9:.1f}s",
                })
        last_seen_ts = ts

    if have_open_session:
        gaps.append({
            "reason": "unclean_shutdown_at_eof",
            "gap_start_ns": last_seen_ts,
            "detail": "no SESSION_STOP at end of input",
        })

    return gaps


def convert(input_glob: str, output_dir: Path, exchange: str, symbol_filter: str | None,
            max_quiet_seconds: float):
    files = sorted(glob.glob(input_glob, recursive=True))
    if not files:
        print(f"error: no files matched {input_glob}", file=sys.stderr)
        sys.exit(1)

    trades_by_day_symbol = defaultdict(list)
    book_by_day_symbol = defaultdict(list)
    book_state: dict[str, OrderBookState] = {}
    events_for_gap_detection = []

    n_frames = n_trades = n_book_updates = n_control = 0
    n_session_start = n_session_stop = n_checkpoint = 0

    for fpath in files:
        for recv_ts_ns, rtype, payload in parse_records(Path(fpath)):
            events_for_gap_detection.append((recv_ts_ns, rtype, None))
            if rtype == REC_SESSION_START:
                n_session_start += 1
                continue
            if rtype == REC_SESSION_STOP:
                n_session_stop += 1
                continue
            if rtype == REC_CHECKPOINT:
                n_checkpoint += 1
                continue
            if rtype != REC_WS_FRAME:
                continue
            n_frames += 1

            try:
                text = payload.decode("utf-8", errors="replace")
            except Exception:
                continue

            for parsed in parse_frame(text, exchange) or []:
                if parsed[0] == "control":
                    n_control += 1
                    continue
                if parsed[0] == "trade":
                    _, inst, ts_ns, px, qty, side = parsed
                    if symbol_filter and inst != symbol_filter:
                        continue
                    n_trades += 1
                    trades_by_day_symbol[(day_key(ts_ns), inst)].append((ts_ns, px, qty, side))
                elif parsed[0] == "book":
                    _, inst, ts_ns, action, bids, asks = parsed
                    if symbol_filter and inst != symbol_filter:
                        continue
                    n_book_updates += 1
                    state = book_state.setdefault(inst, OrderBookState())
                    state.apply(action, bids, asks)
                    top_b, top_a = state.top_n(BOOK_DEPTH)
                    row = {
                        "timestamp_ns": ts_ns,
                        "bid_px": [0.0] * BOOK_DEPTH,
                        "bid_sz": [0.0] * BOOK_DEPTH,
                        "ask_px": [0.0] * BOOK_DEPTH,
                        "ask_sz": [0.0] * BOOK_DEPTH,
                    }
                    for i, (p, s) in enumerate(top_b):
                        row["bid_px"][i] = p
                        row["bid_sz"][i] = s
                    for i, (p, s) in enumerate(top_a):
                        row["ask_px"][i] = p
                        row["ask_sz"][i] = s
                    book_by_day_symbol[(day_key(ts_ns), inst)].append(row)

    print(f"frames: ws={n_frames} (trades={n_trades} books={n_book_updates} ctrl={n_control}) "
          f"sessions={n_session_start} stops={n_session_stop} checkpoints={n_checkpoint}")

    for (day, sym), rows in trades_by_day_symbol.items():
        out = output_dir / "trades" / exchange / sym / f"{day}.parquet"
        write_trades_parquet(out, rows)

    for (day, sym), rows in book_by_day_symbol.items():
        out = output_dir / "orderbook" / exchange / sym / f"{day}.parquet"
        write_book_parquet(out, rows)

    # Gap manifest
    events_for_gap_detection.sort(key=lambda e: e[0])
    gaps = detect_gaps(events_for_gap_detection, int(max_quiet_seconds * 1e9))
    manifest = {
        "exchange": exchange,
        "symbol_filter": symbol_filter,
        "files": files,
        "summary": {
            "ws_frames": n_frames,
            "trades": n_trades,
            "book_updates": n_book_updates,
            "control": n_control,
            "session_starts": n_session_start,
            "session_stops": n_session_stop,
            "checkpoints": n_checkpoint,
            "gaps": len(gaps),
        },
        "gaps": gaps,
    }
    manifest_path = output_dir / "gaps.json"
    output_dir.mkdir(parents=True, exist_ok=True)
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"wrote {manifest_path} ({len(gaps)} gaps)")
    if gaps:
        for g in gaps[:5]:
            print(f"  - {g['reason']}: {g.get('detail', '')}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", required=True, help="Glob for .wslog files (e.g. /opt/bpt/data/raw/okx/**/*.wslog)")
    ap.add_argument("--output", default="/opt/bpt/data/backtest-cache",
                    help="Output Parquet root (bpt-backtester local_cache)")
    ap.add_argument("--exchange", required=True, help="e.g. OKX")
    ap.add_argument("--symbol", default=None,
                    help="Optional exchange-native symbol filter, e.g. BTC-USDT")
    ap.add_argument("--max-quiet-seconds", type=float, default=60.0,
                    help="Flag a gap if WS frames are absent for this long without a checkpoint")
    args = ap.parse_args()

    convert(
        input_glob=args.input,
        output_dir=Path(args.output),
        exchange=args.exchange,
        symbol_filter=args.symbol,
        max_quiet_seconds=args.max_quiet_seconds,
    )


if __name__ == "__main__":
    main()

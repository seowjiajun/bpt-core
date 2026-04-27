#!/usr/bin/env python3
"""Convert raw WS .wslog files (recorded by bpt-md-gateway) to the Parquet
layout bpt-backtester reads. Per-venue dispatch covers OKX / Hyperliquid /
Deribit; HL and Deribit parsers also decode mark / funding / index streams
needed for perp PnL replay fidelity.

File format (little-endian) per record:
    recv_ts_ns u64 | record_type u8 | length u32 | payload bytes (length)

record_type:
    0 = WS_FRAME       (raw venue payload — JSON or JSON-RPC)
    1 = SESSION_START  (config snapshot JSON)
    2 = SESSION_STOP   (exit reason JSON)
    3 = CHECKPOINT     (heartbeat JSON {frames, bytes})
    4 = WS_DISCONNECT  (unexpected connection loss; JSON {reason, attempt})
    5 = WS_RECONNECT   (successful reconnect after disconnect; JSON {attempt})

Output:
    {out}/trades/{exchange}/{symbol}/YYYY-MM-DD.parquet
    {out}/orderbook/{exchange}/{symbol}/YYYY-MM-DD.parquet
    {out}/funding/{exchange}/{symbol}/YYYY-MM-DD.parquet
    {out}/mark/{exchange}/{symbol}/YYYY-MM-DD.parquet
    {out}/index/{exchange}/{index_symbol}/YYYY-MM-DD.parquet   (OKX only)
    {out}/gaps.json             — gap manifest (existing)
    {out}/connection_state.json — list of WS disconnect/reconnect events
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
REC_WS_DISCONNECT = 4
REC_WS_RECONNECT = 5

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


def _f(d: dict, key: str):
    """Float or None from a dict — tolerates missing keys and stringy numbers."""
    v = d.get(key)
    if v is None or v == "":
        return None
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


# ─── OKX ────────────────────────────────────────────────────────────────────

def parse_okx_frame(text: str):
    """Parse one OKX WS JSON frame. Yields tagged tuples; see parse_frame()."""
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
    elif channel.startswith("book") or channel == "bbo-tbt":
        action = msg.get("action", "update")
        for d in (data or []):
            try:
                ts_ns = int(d["ts"]) * 1_000_000
                bids = [(float(b[0]), float(b[1])) for b in (d.get("bids") or [])]
                asks = [(float(a[0]), float(a[1])) for a in (d.get("asks") or [])]
                yield ("book", inst, ts_ns, action, bids, asks)
            except (KeyError, ValueError, TypeError, IndexError):
                continue
    elif channel == "mark-price" and data:
        for d in data:
            try:
                ts_ns = int(d["ts"]) * 1_000_000
                mark_px = float(d["markPx"])
                yield ("mark", inst, ts_ns, mark_px, None, None)
            except (KeyError, ValueError, TypeError):
                continue
    elif channel == "index-tickers" and data:
        for d in data:
            try:
                ts_ns = int(d["ts"]) * 1_000_000
                idx_px = float(d["idxPx"])
                # OKX index is keyed by the index symbol (e.g. BTC-USDT), not
                # an instrument; use that as the "symbol" so the index Parquet
                # is grouped per index, not per perp.
                idx_sym = d.get("instId", inst)
                yield ("index", idx_sym, ts_ns, idx_px)
            except (KeyError, ValueError, TypeError):
                continue
    elif channel == "funding-rate" and data:
        for d in data:
            try:
                ts_ns = int(d["ts"]) * 1_000_000
                fr = float(d["fundingRate"])
                next_ms = d.get("nextFundingTime")
                next_ns = int(next_ms) * 1_000_000 if next_ms else None
                yield ("funding", inst, ts_ns, fr, next_ns)
            except (KeyError, ValueError, TypeError):
                continue


# ─── Hyperliquid ────────────────────────────────────────────────────────────

def parse_hyperliquid_frame(text: str):
    """Parse one HL WS JSON frame.

    Channels handled:
      l2Book          — full snapshot per update (HL semantics)
      trades          — array of trade prints
      activeAssetCtx  — per-asset multiplex of mark, oracle, funding, OI
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
    elif channel == "activeAssetCtx" and isinstance(data, dict):
        try:
            coin = data["coin"]
            ctx = data.get("ctx") or {}
        except KeyError:
            return
        # HL doesn't stamp a per-frame ts inside ctx; use the recv-side ts at
        # the convert() call site instead. We yield a placeholder ts_ns=0 and
        # the caller substitutes recv_ts_ns. (Done in dispatch below.)
        mark = _f(ctx, "markPx")
        oracle = _f(ctx, "oraclePx")
        funding = _f(ctx, "funding")
        oi = _f(ctx, "openInterest")
        if mark is not None or oracle is not None or oi is not None:
            yield ("mark", coin, 0, mark, oracle, oi)
        if funding is not None:
            # HL funding is the one-hour rate, accrues continuously each hour.
            # No "next funding time" — funding is paid hourly on the hour.
            yield ("funding", coin, 0, funding, None)


# ─── Deribit ────────────────────────────────────────────────────────────────

def parse_deribit_frame(text: str):
    """Parse one Deribit WS JSON-RPC notification.

    Frame envelope:
      {"jsonrpc":"2.0","method":"subscription","params":{"channel":"...","data":...}}

    Channels handled:
      trades.{instrument}.100ms
      book.{instrument}.100ms      (snapshot + level-update events)
      quote.{instrument}            (top-of-book; treated as 1-level book)
      ticker.{instrument}.100ms     (mark + index + funding + OI all in one)
    """
    try:
        msg = json.loads(text)
    except (json.JSONDecodeError, ValueError):
        return
    if not isinstance(msg, dict):
        return
    if msg.get("method") != "subscription":
        return  # rpc reply, heartbeat ack, etc.

    params = msg.get("params") or {}
    channel = params.get("channel") or ""
    data = params.get("data")

    if channel.startswith("trades.") and isinstance(data, list):
        for d in data:
            try:
                inst = d["instrument_name"]
                ts_ns = int(d["timestamp"]) * 1_000_000
                px = float(d["price"])
                qty = float(d["amount"])
                side = SIDE_BUY if d.get("direction") == "buy" else SIDE_SELL
                yield ("trade", inst, ts_ns, px, qty, side)
            except (KeyError, ValueError, TypeError):
                continue

    elif channel.startswith("book.") and isinstance(data, dict):
        try:
            inst = data["instrument_name"]
            ts_ns = int(data["timestamp"]) * 1_000_000
            book_type = data.get("type", "change")
            # Deribit deltas are [action, price, size] tuples where action is
            # "new"/"change"/"delete". For our converter, "delete" → size=0
            # so the OrderBookState.apply() update path drops it.
            def _decode_levels(rows):
                out = []
                for row in rows or []:
                    if len(row) == 3:
                        action, p, s = row
                        sz = 0.0 if action == "delete" else float(s)
                        out.append((float(p), sz))
                    elif len(row) == 2:
                        # Snapshot rows are sometimes [price, size] without the action prefix.
                        p, s = row
                        out.append((float(p), float(s)))
                return out
            bids = _decode_levels(data.get("bids"))
            asks = _decode_levels(data.get("asks"))
            action = "snapshot" if book_type == "snapshot" else "update"
            yield ("book", inst, ts_ns, action, bids, asks)
        except (KeyError, ValueError, TypeError):
            return

    elif channel.startswith("quote.") and isinstance(data, dict):
        # quote = top-of-book only; emit as a 1-level snapshot so existing
        # OrderBookState path handles it without a separate code path.
        try:
            inst = data["instrument_name"]
            ts_ns = int(data["timestamp"]) * 1_000_000
            bid_p = _f(data, "best_bid_price")
            bid_s = _f(data, "best_bid_amount")
            ask_p = _f(data, "best_ask_price")
            ask_s = _f(data, "best_ask_amount")
            bids = [(bid_p, bid_s)] if bid_p is not None and bid_s is not None else []
            asks = [(ask_p, ask_s)] if ask_p is not None and ask_s is not None else []
            yield ("book", inst, ts_ns, "snapshot", bids, asks)
        except (KeyError, ValueError, TypeError):
            return

    elif channel.startswith("ticker.") and isinstance(data, dict):
        try:
            inst = data["instrument_name"]
            ts_ns = int(data["timestamp"]) * 1_000_000
        except (KeyError, ValueError, TypeError):
            return
        mark = _f(data, "mark_price")
        index = _f(data, "index_price")
        oi = _f(data, "open_interest")
        if mark is not None or oi is not None:
            yield ("mark", inst, ts_ns, mark, index, oi)
        # Deribit perps publish current_funding (instantaneous rate); options
        # have no funding. funding_8h is the rolling 8h-realized rate, less
        # useful for replay. Use current_funding when present.
        funding = _f(data, "current_funding")
        if funding is not None:
            yield ("funding", inst, ts_ns, funding, None)


# ─── Dispatch ───────────────────────────────────────────────────────────────

def parse_frame(text: str, exchange: str):
    if exchange == "OKX":
        yield from parse_okx_frame(text)
    elif exchange in ("HYPERLIQUID", "HL"):
        yield from parse_hyperliquid_frame(text)
    elif exchange == "DERIBIT":
        yield from parse_deribit_frame(text)
    else:
        raise ValueError(f"unknown --exchange {exchange!r} — expected OKX, HYPERLIQUID, or DERIBIT")


# ─── Order book state ──────────────────────────────────────────────────────

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


# ─── Parquet writers ───────────────────────────────────────────────────────

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


def write_funding_parquet(path: Path, rows: list):
    if not rows:
        return
    rows.sort(key=lambda r: r[0])
    path.parent.mkdir(parents=True, exist_ok=True)
    table = pa.table({
        "timestamp_ns": pa.array([r[0] for r in rows], type=pa.int64()),
        "funding_rate": pa.array([r[1] for r in rows], type=pa.float64()),
        "next_funding_ns": pa.array([r[2] for r in rows], type=pa.int64()),
    })
    pq.write_table(table, path)
    print(f"wrote {path} ({table.num_rows} funding ticks)")


def write_mark_parquet(path: Path, rows: list):
    if not rows:
        return
    rows.sort(key=lambda r: r[0])
    path.parent.mkdir(parents=True, exist_ok=True)
    table = pa.table({
        "timestamp_ns": pa.array([r[0] for r in rows], type=pa.int64()),
        "mark_px": pa.array([r[1] for r in rows], type=pa.float64()),
        "oracle_px": pa.array([r[2] for r in rows], type=pa.float64()),
        "open_interest": pa.array([r[3] for r in rows], type=pa.float64()),
    })
    pq.write_table(table, path)
    print(f"wrote {path} ({table.num_rows} mark ticks)")


def write_index_parquet(path: Path, rows: list):
    if not rows:
        return
    rows.sort(key=lambda r: r[0])
    path.parent.mkdir(parents=True, exist_ok=True)
    table = pa.table({
        "timestamp_ns": pa.array([r[0] for r in rows], type=pa.int64()),
        "index_px": pa.array([r[1] for r in rows], type=pa.float64()),
    })
    pq.write_table(table, path)
    print(f"wrote {path} ({table.num_rows} index ticks)")


# ─── Gap manifest ───────────────────────────────────────────────────────────

def detect_gaps(events: list, max_quiet_ns: int):
    """Walk through (recv_ts, record_type, info) tuples in order and emit gap dicts.

    A gap is any of:
      - SESSION_START without a prior SESSION_STOP — previous session ended uncleanly
      - Two records >max_quiet_ns apart with no intervening CHECKPOINT
      - A file ends without a SESSION_STOP record
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


def _ts_iso(ns: int) -> str:
    return datetime.fromtimestamp(ns / 1e9, tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


# ─── Main convert loop ─────────────────────────────────────────────────────

def convert(input_glob: str, output_dir: Path, exchange: str, symbol_filter: str | None,
            max_quiet_seconds: float):
    files = sorted(glob.glob(input_glob, recursive=True))
    if not files:
        print(f"error: no files matched {input_glob}", file=sys.stderr)
        sys.exit(1)

    trades_by_day_symbol = defaultdict(list)
    book_by_day_symbol = defaultdict(list)
    funding_by_day_symbol = defaultdict(list)
    mark_by_day_symbol = defaultdict(list)
    index_by_day_symbol = defaultdict(list)
    book_state: dict[str, OrderBookState] = {}
    events_for_gap_detection = []
    connection_events = []  # WS_DISCONNECT / WS_RECONNECT

    counts = {"frames": 0, "trades": 0, "books": 0, "funding": 0, "mark": 0, "index": 0,
              "session_start": 0, "session_stop": 0, "checkpoint": 0,
              "ws_disconnect": 0, "ws_reconnect": 0}

    for fpath in files:
        for recv_ts_ns, rtype, payload in parse_records(Path(fpath)):
            events_for_gap_detection.append((recv_ts_ns, rtype, None))
            if rtype == REC_SESSION_START:
                counts["session_start"] += 1
                continue
            if rtype == REC_SESSION_STOP:
                counts["session_stop"] += 1
                continue
            if rtype == REC_CHECKPOINT:
                counts["checkpoint"] += 1
                continue
            if rtype == REC_WS_DISCONNECT:
                counts["ws_disconnect"] += 1
                try:
                    info = json.loads(payload.decode("utf-8", errors="replace"))
                except (json.JSONDecodeError, ValueError):
                    info = {}
                connection_events.append({
                    "ts": _ts_iso(recv_ts_ns),
                    "ts_ns": recv_ts_ns,
                    "event": "disconnect",
                    "reason": info.get("reason"),
                    "attempt": info.get("attempt"),
                })
                continue
            if rtype == REC_WS_RECONNECT:
                counts["ws_reconnect"] += 1
                try:
                    info = json.loads(payload.decode("utf-8", errors="replace"))
                except (json.JSONDecodeError, ValueError):
                    info = {}
                connection_events.append({
                    "ts": _ts_iso(recv_ts_ns),
                    "ts_ns": recv_ts_ns,
                    "event": "reconnect",
                    "attempt": info.get("attempt"),
                })
                continue
            if rtype != REC_WS_FRAME:
                continue
            counts["frames"] += 1

            try:
                text = payload.decode("utf-8", errors="replace")
            except Exception:
                continue

            for parsed in parse_frame(text, exchange) or []:
                kind = parsed[0]
                if kind == "trade":
                    _, inst, ts_ns, px, qty, side = parsed
                    if symbol_filter and inst != symbol_filter:
                        continue
                    counts["trades"] += 1
                    trades_by_day_symbol[(day_key(ts_ns), inst)].append((ts_ns, px, qty, side))
                elif kind == "book":
                    _, inst, ts_ns, action, bids, asks = parsed
                    if symbol_filter and inst != symbol_filter:
                        continue
                    counts["books"] += 1
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
                elif kind == "funding":
                    _, inst, ts_ns, fr, next_ns = parsed
                    # HL frames carry no per-frame ts inside ctx — fall back
                    # to recv_ts_ns. Same for any future venue that adds the
                    # placeholder convention.
                    if ts_ns == 0:
                        ts_ns = recv_ts_ns
                    if symbol_filter and inst != symbol_filter:
                        continue
                    counts["funding"] += 1
                    funding_by_day_symbol[(day_key(ts_ns), inst)].append(
                        (ts_ns, fr, next_ns if next_ns is not None else 0))
                elif kind == "mark":
                    _, inst, ts_ns, mark_px, oracle_px, oi = parsed
                    if ts_ns == 0:
                        ts_ns = recv_ts_ns
                    if symbol_filter and inst != symbol_filter:
                        continue
                    counts["mark"] += 1
                    mark_by_day_symbol[(day_key(ts_ns), inst)].append(
                        (ts_ns,
                         mark_px if mark_px is not None else float("nan"),
                         oracle_px if oracle_px is not None else float("nan"),
                         oi if oi is not None else float("nan")))
                elif kind == "index":
                    _, idx_sym, ts_ns, idx_px = parsed
                    if symbol_filter and idx_sym != symbol_filter:
                        continue
                    counts["index"] += 1
                    index_by_day_symbol[(day_key(ts_ns), idx_sym)].append((ts_ns, idx_px))

    print(
        "frames: ws={frames} (trades={trades} books={books} mark={mark} "
        "funding={funding} index={index}) sessions={session_start} "
        "stops={session_stop} checkpoints={checkpoint} "
        "disconnects={ws_disconnect} reconnects={ws_reconnect}".format(**counts))

    for (day, sym), rows in trades_by_day_symbol.items():
        write_trades_parquet(output_dir / "trades" / exchange / sym / f"{day}.parquet", rows)
    for (day, sym), rows in book_by_day_symbol.items():
        write_book_parquet(output_dir / "orderbook" / exchange / sym / f"{day}.parquet", rows)
    for (day, sym), rows in funding_by_day_symbol.items():
        write_funding_parquet(output_dir / "funding" / exchange / sym / f"{day}.parquet", rows)
    for (day, sym), rows in mark_by_day_symbol.items():
        write_mark_parquet(output_dir / "mark" / exchange / sym / f"{day}.parquet", rows)
    for (day, sym), rows in index_by_day_symbol.items():
        write_index_parquet(output_dir / "index" / exchange / sym / f"{day}.parquet", rows)

    # Gap manifest
    events_for_gap_detection.sort(key=lambda e: e[0])
    gaps = detect_gaps(events_for_gap_detection, int(max_quiet_seconds * 1e9))
    manifest = {
        "exchange": exchange,
        "symbol_filter": symbol_filter,
        "files": files,
        "summary": {
            "ws_frames": counts["frames"],
            "trades": counts["trades"],
            "book_updates": counts["books"],
            "mark_updates": counts["mark"],
            "funding_updates": counts["funding"],
            "index_updates": counts["index"],
            "session_starts": counts["session_start"],
            "session_stops": counts["session_stop"],
            "checkpoints": counts["checkpoint"],
            "ws_disconnects": counts["ws_disconnect"],
            "ws_reconnects": counts["ws_reconnect"],
            "gaps": len(gaps),
        },
        "gaps": gaps,
    }
    output_dir.mkdir(parents=True, exist_ok=True)
    with open(output_dir / "gaps.json", "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"wrote {output_dir / 'gaps.json'} ({len(gaps)} gaps)")
    if gaps:
        for g in gaps[:5]:
            print(f"  - {g['reason']}: {g.get('detail', '')}")

    if connection_events:
        connection_events.sort(key=lambda e: e["ts_ns"])
        with open(output_dir / "connection_state.json", "w") as f:
            json.dump(connection_events, f, indent=2)
        print(f"wrote {output_dir / 'connection_state.json'} "
              f"({counts['ws_disconnect']} disconnects, {counts['ws_reconnect']} reconnects)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--input", required=True, help="Glob for .wslog files (e.g. /opt/bpt/data/raw/okx/**/*.wslog)")
    ap.add_argument("--output", default="/opt/bpt/data/backtest-cache",
                    help="Output Parquet root (bpt-backtester local_cache)")
    ap.add_argument("--exchange", required=True, choices=["OKX", "HYPERLIQUID", "HL", "DERIBIT"],
                    help="Venue parser dispatch")
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

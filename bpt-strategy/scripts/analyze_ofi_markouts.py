#!/usr/bin/env python3
"""Post-hoc adverse-selection analysis for OFI strategy fenrir logs.

Links each ENTER log line (signal strength, mid, spread at decision time)
to its corresponding ENTRY markout via the next FILLED order_id, then bins
the resulting (features, markout) pairs to look for adverse-selection
patterns:

  - markout vs signal strength (stronger signal → worse markout = red flag)
  - markout vs spread at entry
  - markout vs short-term realized vol leading into the entry
  - markout conditional on signal still being strong at t+1s

Single-instrument assumption: ENTER lines and subsequent FILLED order_ids
are paired in time order. Fenrir's OFI strategy serializes order flow
(one in-flight order at a time), so this is safe for the current config.

Usage:
    python3 fenrir/scripts/analyze_ofi_markouts.py [fenrir/logs/fenrir.log]
"""
import argparse
import re
import statistics
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Optional


TS = r"(\d{2}:\d{2}:\d{2}\.\d+)"
ENTER_RE = re.compile(
    rf"^{TS}.*\[OFI\] (\S+) ENTER (\w+) ofi=(-?[\d.]+) mid=([\d.]+) spread=([\d.]+)bps"
)
FILLED_RE = re.compile(
    rf"^{TS}.*\[OFI\] (\S+) order_id=(\d+) FILLED"
)
MARKOUT_RE = re.compile(
    rf"^{TS}.*\[OFI markout\] (\S+) order_id=(\d+) kind=(\w+) side=(\w+) "
    rf"fill=([\d.]+) mid=([\d.]+) t\+(\d+)s=(-?[\d.]+)bps"
)


def ts_to_ns(ts: str) -> int:
    h, m, rest = ts.split(":")
    s, frac = rest.split(".")
    frac = (frac + "000000000")[:9]
    return ((int(h) * 3600 + int(m) * 60 + int(s)) * 1_000_000_000) + int(frac)


@dataclass
class Trade:
    order_id: int = 0
    direction: str = ""           # "LONG" or "SHORT"
    ofi_at_entry: float = 0.0
    mid_at_entry: float = 0.0
    spread_at_entry: float = 0.0
    entry_ns: int = 0
    fill_ns: int = 0
    markouts: dict = field(default_factory=dict)  # {horizon: bps}


def parse_log(path: str) -> list[Trade]:
    """Walk the log and pair ENTER lines with their FILLED order_ids + ENTRY markouts."""
    trades: list[Trade] = []
    pending_enter: Optional[Trade] = None
    active_entry_oids: set[int] = set()
    oid_to_trade: dict[int, Trade] = {}

    # Track recent mid history for realized-vol feature. (ts_ns, mid)
    mid_hist: list[tuple[int, float]] = []

    with open(path) as f:
        for line in f:
            m = ENTER_RE.match(line)
            if m:
                ts, sym, side, ofi, mid, spread = m.groups()
                t = Trade(
                    direction=side,
                    ofi_at_entry=float(ofi),
                    mid_at_entry=float(mid),
                    spread_at_entry=float(spread),
                    entry_ns=ts_to_ns(ts),
                )
                pending_enter = t
                continue

            m = FILLED_RE.match(line)
            if m and pending_enter is not None:
                ts, sym, oid = m.groups()
                t = pending_enter
                t.order_id = int(oid)
                t.fill_ns = ts_to_ns(ts)
                oid_to_trade[t.order_id] = t
                active_entry_oids.add(t.order_id)
                trades.append(t)
                pending_enter = None
                continue

            m = MARKOUT_RE.match(line)
            if m:
                ts, sym, oid, kind, side, fill, mid, horizon, bps = m.groups()
                if kind != "ENTRY":
                    continue
                oid_i = int(oid)
                t = oid_to_trade.get(oid_i)
                if t is None:
                    continue
                t.markouts[int(horizon)] = float(bps)
                continue

    return trades


def bin_stats(label: str, buckets: dict, horizon: int = 1) -> None:
    print(f"\n=== {label} | t+{horizon}s markout (bps) ===")
    print(f"{'bucket':<24}{'n':>5}{'mean':>9}{'median':>9}{'p25':>9}{'p75':>9}")
    for key in sorted(buckets):
        vals = sorted(v.markouts.get(horizon) for v in buckets[key] if horizon in v.markouts)
        vals = [v for v in vals if v is not None]
        if not vals:
            continue
        n = len(vals)
        def pct(p): return vals[min(n - 1, int(n * p))]
        print(
            f"{key!s:<24}{n:>5}{statistics.mean(vals):>9.2f}"
            f"{pct(0.5):>9.2f}{pct(0.25):>9.2f}{pct(0.75):>9.2f}"
        )


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", default="fenrir/logs/fenrir.log")
    args = ap.parse_args()

    trades = parse_log(args.log)
    trades_with_m1 = [t for t in trades if 1 in t.markouts]
    trades_with_m30 = [t for t in trades if 30 in t.markouts]
    print(f"Parsed {len(trades)} entries "
          f"({len(trades_with_m1)} have t+1s, {len(trades_with_m30)} have t+30s)")

    if not trades_with_m1:
        print("No entries with t+1s markouts yet — need more runtime.")
        return 1

    # ─────────────────────────────────────────────────────────────────
    # Bin 1: signal strength at entry
    # Adverse selection shows as: stronger signal → WORSE markout.
    # Healthy signal shows as:    stronger signal → better markout.
    # ─────────────────────────────────────────────────────────────────
    def signal_bucket(t: Trade) -> str:
        a = abs(t.ofi_at_entry)
        if a < 1.25: return "(1) 1.00-1.25"
        if a < 1.75: return "(2) 1.25-1.75"
        if a < 2.50: return "(3) 1.75-2.50"
        return "(4) >=2.50"

    by_signal = defaultdict(list)
    for t in trades_with_m1:
        by_signal[signal_bucket(t)].append(t)
    bin_stats("signal strength |OFI|", by_signal, horizon=1)
    bin_stats("signal strength |OFI|", by_signal, horizon=30)

    # ─────────────────────────────────────────────────────────────────
    # Bin 2: spread at entry. Wider spread = less certain signal.
    # ─────────────────────────────────────────────────────────────────
    def spread_bucket(t: Trade) -> str:
        s = t.spread_at_entry
        if s < 0.25: return "(1) <0.25"
        if s < 0.5:  return "(2) 0.25-0.5"
        if s < 1.0:  return "(3) 0.5-1.0"
        return "(4) >=1.0"

    by_spread = defaultdict(list)
    for t in trades_with_m1:
        by_spread[spread_bucket(t)].append(t)
    bin_stats("spread at entry (bps)", by_spread, horizon=1)

    # ─────────────────────────────────────────────────────────────────
    # Bin 3: by direction — LONG vs SHORT asymmetry
    # ─────────────────────────────────────────────────────────────────
    by_dir = defaultdict(list)
    for t in trades_with_m1:
        by_dir[t.direction].append(t)
    bin_stats("direction", by_dir, horizon=1)
    bin_stats("direction", by_dir, horizon=30)

    # ─────────────────────────────────────────────────────────────────
    # Summary: correlation between |signal| and t+1s markout.
    # Negative correlation is the adverse-selection signature.
    # ─────────────────────────────────────────────────────────────────
    xs = [abs(t.ofi_at_entry) for t in trades_with_m1]
    ys = [t.markouts[1] for t in trades_with_m1]
    if len(xs) >= 5:
        n = len(xs)
        mx = sum(xs) / n
        my = sum(ys) / n
        cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / n
        vx = sum((x - mx) ** 2 for x in xs) / n
        vy = sum((y - my) ** 2 for y in ys) / n
        corr = cov / ((vx * vy) ** 0.5) if vx > 0 and vy > 0 else float("nan")
        print(f"\ncorr(|ofi|, t+1s markout) = {corr:+.3f}  (negative = adverse selection)")

    return 0


if __name__ == "__main__":
    sys.exit(main())

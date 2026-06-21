"""Hurst regime IC analysis — does Hurst predict momentum vs reversion?

Pure-python: bpt_canon reader (no _core.so) + numpy/pandas Hurst (R/S).

Method:
  - Read BBO mid for an instrument, resample to fixed bars
  - Rolling Hurst H_t over a lookback window (R/S analysis)
  - For each bar: past_return r[t-h..t], forward_return r[t..t+h]
  - Bucket (past, forward) pairs by contemporaneous Hurst regime
  - Within each bucket measure corr(past, forward):
      high-H bucket positive  -> trending/momentum regime
      low-H  bucket negative  -> mean-reverting regime
  - No look-ahead: H_t and past use data <= t; fwd is strictly future.

Swept across (day, instrument) to separate real signal from in-sample luck.
"""
import sys
from pathlib import Path

import numpy as np
import pandas as pd

REPO = Path("/home/jseow/code/bpt-core")
sys.path.insert(0, str(REPO / "bpt-canon" / "python"))
import bpt_canon as bc  # noqa: E402

CANON_DIR = Path("/tmp/hurst_canon")
DAYS = ["2026-05-05", "2026-05-06", "2026-05-07"]
INSTRUMENTS = {"BTC": 1001, "ETH": 1002, "SOL": 1003}
BAR = "10s"
HURST_WIN = 200
HORIZON = 30


def rs_hurst(x: np.ndarray) -> float:
    x = x[~np.isnan(x)]
    n = len(x)
    if n < 32:
        return np.nan
    sizes = []
    s = 8
    while s <= n // 2:
        sizes.append(s)
        s *= 2
    if len(sizes) < 3:
        return np.nan
    rs_vals = []
    for sz in sizes:
        n_chunks = n // sz
        rs_chunk = []
        for i in range(n_chunks):
            seg = x[i * sz:(i + 1) * sz]
            dev = np.cumsum(seg - seg.mean())
            R = dev.max() - dev.min()
            S = seg.std(ddof=0)
            if S > 0:
                rs_chunk.append(R / S)
        if rs_chunk:
            rs_vals.append(np.mean(rs_chunk))
    if len(rs_vals) < 3:
        return np.nan
    logs = np.log(np.array(sizes[:len(rs_vals)]))
    logrs = np.log(np.array(rs_vals))
    return float(np.polyfit(logs, logrs, 1)[0])


def analyse(canon: Path, iid: int):
    try:
        bbo = bc.read_bbos(canon)
    except ValueError:
        return "incomplete"
    bbo = bbo[bbo.instrument_id == iid].copy()
    if len(bbo) < 5000:
        return None
    bbo = bbo[(bbo.bid > 0) & (bbo.ask > 0) & (bbo.ask >= bbo.bid)]
    bbo["mid"] = (bbo.bid + bbo.ask) * 0.5
    bbo["ts"] = pd.to_datetime(bbo.ts_ns, unit="ns")
    bbo = bbo.set_index("ts").sort_index()
    bars = bbo["mid"].resample(BAR).last().ffill().dropna()
    logret = np.log(bars / bars.shift(1)).fillna(0.0).values
    n = len(logret)
    if n < HURST_WIN + 2 * HORIZON + 50:
        return None

    H = np.full(n, np.nan)
    for t in range(HURST_WIN, n):
        H[t] = rs_hurst(logret[t - HURST_WIN:t])
    cum = np.concatenate([[0.0], np.cumsum(logret)])
    past = np.full(n, np.nan)
    fwd = np.full(n, np.nan)
    for t in range(HORIZON, n - HORIZON):
        past[t] = cum[t + 1] - cum[t + 1 - HORIZON]
        fwd[t] = cum[t + 1 + HORIZON] - cum[t + 1]

    df = pd.DataFrame({"H": H, "past": past, "fwd": fwd}).dropna()
    if len(df) < 300:
        return None
    df["bucket"] = pd.qcut(df.H, 3, labels=["low", "mid", "high"])
    res = {}
    for b in ("low", "mid", "high"):
        g = df[df.bucket == b]
        res[b] = (len(g), g.H.mean(), np.corrcoef(g.past, g.fwd)[0, 1])
    res["spread"] = res["high"][2] - res["low"][2]
    res["n"] = len(df)
    return res


def main():
    rows = []
    print(f"{'day':>12} {'inst':>5} {'n':>6} "
          f"{'low_H':>6} {'low_c':>8} {'mid_c':>8} {'high_H':>6} {'high_c':>8} "
          f"{'spread':>8}", flush=True)
    for day in DAYS:
        canon = CANON_DIR / f"hl-{day}.canon"
        if not canon.exists():
            print(f"{day:>12}  (canon missing, skip)")
            continue
        for name, iid in INSTRUMENTS.items():
            r = analyse(canon, iid)
            if r == "incomplete":
                print(f"{day:>12} {name:>5}  (canon mid-write, skip)")
                continue
            if r is None:
                print(f"{day:>12} {name:>5}  (insufficient data)")
                continue
            print(f"{day:>12} {name:>5} {r['n']:>6} "
                  f"{r['low'][1]:>6.3f} {r['low'][2]:>+8.4f} {r['mid'][2]:>+8.4f} "
                  f"{r['high'][1]:>6.3f} {r['high'][2]:>+8.4f} {r['spread']:>+8.4f}",
                  flush=True)
            rows.append((day, name, r))

    print("\n=== summary ===")
    spreads = [r["spread"] for _, _, r in rows]
    lows = [r["low"][2] for _, _, r in rows]
    highs = [r["high"][2] for _, _, r in rows]
    if spreads:
        print(f"cells: {len(spreads)}")
        print(f"low-H corr  : mean {np.mean(lows):+.4f}  (want NEGATIVE = reversion)")
        print(f"high-H corr : mean {np.mean(highs):+.4f}  (want POSITIVE = momentum)")
        print(f"spread      : mean {np.mean(spreads):+.4f}  min {min(spreads):+.4f}  "
              f"max {max(spreads):+.4f}")
        n_pos = sum(1 for s in spreads if s > 0)
        n_lo_neg = sum(1 for v in lows if v < 0)
        print(f"spread > 0 in {n_pos}/{len(spreads)} cells; "
              f"low-H reverts (corr<0) in {n_lo_neg}/{len(lows)} cells")
        print("\nRobust if the sign holds across MOST (day,inst) cells, not just ETH/05-07.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

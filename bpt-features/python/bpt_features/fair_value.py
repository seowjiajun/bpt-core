"""Function-style wrappers for the FairValueEstimator C++ class.

FairValueEstimator is a family of reference-price estimators selected
via Config::mode. Rather than make Python users construct a Config
object and pick a Mode enum, we expose one function per mode — the
typical research call site reads as "give me the microprice" or "give
me the L2-weighted fair value", not "give me the FairValueEstimator
in microprice mode".

All wrappers take the same DataFrame schema (canon reader output):
  ts_ns: uint64
  bid:   float64  (best bid price)
  ask:   float64  (best ask price)
  bid_qty: float64 (best bid size)
  ask_qty: float64 (best ask size)
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from bpt_features._core import FairValueEstimator

if TYPE_CHECKING:
    import pandas as pd


def _run(df: "pd.DataFrame", cfg, name: str,
         bid_col: str, ask_col: str, bid_qty_col: str, ask_qty_col: str) -> "pd.Series":
    """Shared inner loop — construct estimator + feed each row."""
    import pandas as pd

    fve = FairValueEstimator(cfg)
    out = [
        fve.estimate(bid, ask, bq, aq)
        for bid, ask, bq, aq in zip(df[bid_col], df[ask_col], df[bid_qty_col], df[ask_qty_col])
    ]
    return pd.Series(out, index=df.index, name=name)


def mid_price(df: "pd.DataFrame", *,
              bid_col: str = "bid", ask_col: str = "ask",
              bid_qty_col: str = "bid_qty", ask_qty_col: str = "ask_qty") -> "pd.Series":
    """(bid + ask) / 2. Naive baseline."""
    cfg = FairValueEstimator.Config()
    cfg.mode = FairValueEstimator.Mode.Mid
    return _run(df, cfg, "mid", bid_col, ask_col, bid_qty_col, ask_qty_col)


def microprice(df: "pd.DataFrame", *,
               bid_col: str = "bid", ask_col: str = "ask",
               bid_qty_col: str = "bid_qty", ask_qty_col: str = "ask_qty") -> "pd.Series":
    """Stoikov micro-price: (bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty).

    Tilts toward the thinner side — that side is the one about to move
    first. Closer to a true martingale than the mid for most books.
    """
    cfg = FairValueEstimator.Config()
    cfg.mode = FairValueEstimator.Mode.Micro
    return _run(df, cfg, "microprice", bid_col, ask_col, bid_qty_col, ask_qty_col)


def microprice_size_capped(df: "pd.DataFrame", *, size_cap_qty: float,
                            bid_col: str = "bid", ask_col: str = "ask",
                            bid_qty_col: str = "bid_qty", ask_qty_col: str = "ask_qty"
                            ) -> "pd.Series":
    """Microprice with per-side qty clamped to `size_cap_qty`.

    Defends against displayed-size manipulation where one side momentarily
    shows a fat queue larger than its true intent (iceberg / spoofing).
    """
    cfg = FairValueEstimator.Config()
    cfg.mode = FairValueEstimator.Mode.MicroSizeCapped
    cfg.size_cap_qty = size_cap_qty
    return _run(df, cfg, "microprice_capped", bid_col, ask_col, bid_qty_col, ask_qty_col)


def fair_value_ewma(df: "pd.DataFrame", *, alpha: float = 0.3,
                    bid_col: str = "bid", ask_col: str = "ask",
                    bid_qty_col: str = "bid_qty", ask_qty_col: str = "ask_qty"
                    ) -> "pd.Series":
    """EWMA-smoothed microprice: alpha * micro + (1-alpha) * previous.

    Use when raw micro is too jittery for your re-quote cadence. Adds
    one tick of lag.
    """
    cfg = FairValueEstimator.Config()
    cfg.mode = FairValueEstimator.Mode.EwmaMicro
    cfg.ewma_alpha = alpha
    return _run(df, cfg, "fair_value_ewma", bid_col, ask_col, bid_qty_col, ask_qty_col)

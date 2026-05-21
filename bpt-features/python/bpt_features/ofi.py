"""Function-style wrapper for the OFICalculator C++ class.

Research convention: take a DataFrame, return a Series. Internally
creates the streaming OFICalculator + loops over the rows feeding it
book snapshots in order.

This is the simplest possible wrapper — one Python loop calling C++
once per row. Acceptable for research at canon scale (~10s per
day-of-data). If a feature becomes hot enough that the per-row Python
overhead matters, the fix is a batch C++ method on the underlying
class, not a vectorised Python re-implementation (which would
re-introduce production-vs-research drift).
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from bpt_features._core import OFICalculator

if TYPE_CHECKING:
    import pandas as pd


def ofi(
    df: "pd.DataFrame",
    *,
    window_ns: int = 1_000_000_000,
    max_levels: int = 5,
    ts_col: str = "ts_ns",
    bids_col: str = "bids",
    asks_col: str = "asks",
) -> "pd.Series":
    """Compute rolling OFI over a canon DataFrame.

    Parameters
    ----------
    df :
        Canon DataFrame. Expected columns: ts_ns (uint64), bids (list of
        (price, qty) tuples), asks (same). The canon reader emits these.
    window_ns :
        Rolling window for the OFI sum, default 1 second. Matches the
        OFICalculator.Config default.
    max_levels :
        Max book levels to aggregate, default 5.
    ts_col / bids_col / asks_col :
        Column names — override if your DataFrame uses different names.

    Returns
    -------
    pandas Series of OFI values, one per row, named "ofi".
    Indexed identically to the input DataFrame.
    """
    import pandas as pd

    cfg = OFICalculator.Config()
    cfg.max_levels = max_levels
    cfg.window_ns = window_ns
    calc = OFICalculator(cfg)

    out = [
        calc.update(bids, asks, ts)
        for bids, asks, ts in zip(df[bids_col], df[asks_col], df[ts_col])
    ]
    return pd.Series(out, index=df.index, name="ofi")

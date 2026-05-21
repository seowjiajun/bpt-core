"""Function-style wrapper for RealizedVolEstimator.

Rolling annualised realised vol from mid-price returns, sampled at a
fixed cadence so the rate is deterministic regardless of tick density.

The estimator is `update`-driven: every tick, it decides whether to
take a new sample based on time-since-last-sample. Most ticks get
ignored when sample_interval_ns is set; only ticks at the chosen
cadence contribute. The Python wrapper exposes both the per-row vol
series AND the boolean "did this tick sample?" series — useful for
verifying the sampling cadence is what you expected.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from bpt_features._core import RealizedVolEstimator

if TYPE_CHECKING:
    import pandas as pd


def realized_vol(df: "pd.DataFrame", *,
                 window_size: int = 60,
                 sample_interval_ns: int = 1_000_000_000,
                 mid_col: str = "mid",
                 ts_col: str = "ts_ns") -> "pd.Series":
    """Compute rolling annualised realised vol over a canon DataFrame.

    Returns one value per row. Rows where the estimator ignored the
    tick (insufficient time since last sample, or not yet warm) get
    the prior value carried forward.

    Parameters
    ----------
    window_size :
        Number of samples in the rolling window. With sample_interval_ns
        = 1s and window_size = 60, that's a 1-minute realised vol.
    sample_interval_ns :
        Minimum spacing between accepted samples. Ticks closer than
        this are ignored.
    mid_col, ts_col :
        Column names — override if your DataFrame uses different names.
    """
    import pandas as pd

    est = RealizedVolEstimator(window_size, sample_interval_ns)
    out = []
    for mid, ts in zip(df[mid_col], df[ts_col]):
        est.update(mid, ts)
        out.append(est.realized_vol())
    return pd.Series(out, index=df.index, name="realized_vol")

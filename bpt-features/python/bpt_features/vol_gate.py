"""Function-style wrapper for VolatilityGate.

VolatilityGate is a binary signal — it returns True when the recent
mid-price move exceeded `max_bps_per_window` (suggesting a vol spike
worth pausing on). Strategies use it as a "stop quoting" gate.

For research, the natural use is "at each tick, would the gate have
been halted?" — returns a bool series the same length as the input.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from bpt_features._core import VolatilityGate

if TYPE_CHECKING:
    import pandas as pd


def vol_gate(df: "pd.DataFrame", *,
             max_bps_per_window: float,
             mid_col: str = "mid",
             ts_col: str = "ts_ns") -> "pd.Series":
    """Compute vol-gate halted-state over a canon DataFrame.

    Parameters
    ----------
    max_bps_per_window :
        Bps move within the gate's internal window that trips the gate.
        Set to a value smaller than typical quiet-market moves and
        larger than typical noise — e.g. 20 bps for crypto perps.
    mid_col, ts_col :
        Column names — override if your DataFrame uses different names.

    Returns
    -------
    Boolean Series: True where the gate is HALTED, False where it's
    permitting trading.
    """
    import pandas as pd

    cfg = VolatilityGate.Config()
    cfg.max_bps_per_window = max_bps_per_window
    gate = VolatilityGate(cfg)
    out = [
        gate.update_and_check(mid, ts)
        for mid, ts in zip(df[mid_col], df[ts_col])
    ]
    return pd.Series(out, index=df.index, name="vol_gate_halted", dtype=bool)

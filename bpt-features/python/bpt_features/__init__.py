"""bpt-features — production-grade microstructure feature library.

Compiled C++ implementations exposed to Python via pybind11. The same
code that runs in production strategies runs in research notebooks —
no separate Python implementation, no drift.

Usage:
    import bpt_features as bf

    # Raw class API — streaming, mirrors C++:
    cfg = bf.OFICalculator.Config()
    calc = bf.OFICalculator(cfg)
    val = calc.update(bids=[(100.0, 5.0)], asks=[(100.1, 3.0)], timestamp_ns=ts)

    # Function-style wrappers for research (df → Series):
    df["ofi"]      = bf.ofi(df, window_ns=1_000_000_000)
    df["micro"]    = bf.microprice(df)
    df["mid"]      = bf.mid_price(df)
    df["rv_60s"]   = bf.realized_vol(df, window_size=60, sample_interval_ns=1_000_000_000)
    df["vol_halt"] = bf.vol_gate(df, max_bps_per_window=20.0)
"""

# Re-export the compiled classes verbatim.
from bpt_features._core import (  # noqa: F401
    FairValueEstimator,
    OFICalculator,
    RealizedVolEstimator,
    VolatilityGate,
)

# Function-style wrappers for the typical research idiom.
from bpt_features.fair_value import (  # noqa: F401
    fair_value_ewma,
    microprice,
    microprice_size_capped,
    mid_price,
)
from bpt_features.ofi import ofi  # noqa: F401
from bpt_features.realized_vol import realized_vol  # noqa: F401
from bpt_features.vol_gate import vol_gate  # noqa: F401

__all__ = [
    # Raw C++ classes
    "FairValueEstimator",
    "OFICalculator",
    "RealizedVolEstimator",
    "VolatilityGate",
    # Function-style wrappers
    "fair_value_ewma",
    "microprice",
    "microprice_size_capped",
    "mid_price",
    "ofi",
    "realized_vol",
    "vol_gate",
]

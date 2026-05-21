"""bpt-features — production-grade microstructure feature library.

Compiled C++ implementations exposed to Python via pybind11. The same
code that runs in production strategies runs in research notebooks —
no separate Python implementation, no drift.

Usage:
    import bpt_features as bf

    # Raw class — streaming, mirrors C++ API:
    cfg = bf.OFICalculator.Config()
    cfg.max_levels = 5
    cfg.window_ns = 1_000_000_000
    ofi = bf.OFICalculator(cfg)
    val = ofi.update(bids=[(100.0, 5.0)], asks=[(100.1, 3.0)], timestamp_ns=ts)

    # Function-style wrappers for research (df → Series):
    df['ofi'] = bf.ofi(df, window_ns=1_000_000_000, max_levels=5)
"""

# Re-export the compiled classes verbatim.
from bpt_features._core import OFICalculator  # noqa: F401

# Function-style wrappers for the typical research idiom.
from bpt_features.ofi import ofi  # noqa: F401

__all__ = [
    "OFICalculator",
    "ofi",
]

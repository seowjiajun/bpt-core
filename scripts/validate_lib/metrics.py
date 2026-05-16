"""Statistical comparison helpers for sim-vs-prod validation.

Pure-stdlib (no scipy/numpy) so the harness runs anywhere a python ≥ 3.11
exists. KS distance is the only non-trivial test; the rest are scalar
diffs.
"""

from __future__ import annotations

import math
from dataclasses import dataclass


@dataclass(frozen=True)
class ScalarDiff:
    """Diff between two scalars. Both absolute and relative — pick whichever
    is appropriate for the threshold check (relative is degenerate at 0)."""

    sim: float
    prod: float

    @property
    def abs_diff(self) -> float:
        return self.sim - self.prod

    @property
    def rel_diff(self) -> float:
        if self.prod == 0:
            return float("inf") if self.sim != 0 else 0.0
        return (self.sim - self.prod) / self.prod


@dataclass(frozen=True)
class DistributionDiff:
    """Comparison of two empirical distributions via KS distance.

    KS distance = sup_x |F_sim(x) − F_prod(x)| where F is the empirical
    CDF. Bounded in [0, 1]; 0 = identical distribution shapes, 1 = fully
    disjoint supports. Sample size shows up implicitly: tiny samples have
    inflated noise floors.
    """

    sim_n: int
    prod_n: int
    sim_mean: float
    prod_mean: float
    ks: float


def ks_distance(a: list[float], b: list[float]) -> float:
    """Two-sample Kolmogorov–Smirnov statistic.

    Algorithm: merge-walk over the sorted union of values, tracking each
    side's empirical CDF and recording the running max gap. O((n+m) log
    (n+m)) for the sort, O(n+m) for the walk. No external deps.

    Returns 0.0 when both inputs are empty (degenerate case — no
    information either way; downstream caller should treat as not-comparable).
    """
    if not a and not b:
        return 0.0
    if not a or not b:
        return 1.0

    sa = sorted(a)
    sb = sorted(b)
    na = len(sa)
    nb = len(sb)

    i = j = 0
    cdf_a = cdf_b = 0.0
    ks = 0.0

    while i < na or j < nb:
        # Pick the smaller next value to advance both CDFs uniformly.
        if i < na and (j >= nb or sa[i] <= sb[j]):
            v = sa[i]
            # Consume all duplicates of v in a.
            while i < na and sa[i] == v:
                i += 1
                cdf_a = i / na
            while j < nb and sb[j] == v:
                j += 1
                cdf_b = j / nb
        else:
            v = sb[j]
            while j < nb and sb[j] == v:
                j += 1
                cdf_b = j / nb
            while i < na and sa[i] == v:
                i += 1
                cdf_a = i / na
        gap = abs(cdf_a - cdf_b)
        if gap > ks:
            ks = gap
    return ks


def distribution_diff(sim: list[float], prod: list[float]) -> DistributionDiff:
    sim_mean = sum(sim) / len(sim) if sim else 0.0
    prod_mean = sum(prod) / len(prod) if prod else 0.0
    return DistributionDiff(
        sim_n=len(sim),
        prod_n=len(prod),
        sim_mean=sim_mean,
        prod_mean=prod_mean,
        ks=ks_distance(sim, prod),
    )


def relative_count_diff(sim_n: int, prod_n: int) -> float:
    """`(sim - prod) / max(prod, 1)`. Bounded to avoid div-by-zero
    when either run produced zero fills."""
    if prod_n == 0:
        return float("inf") if sim_n != 0 else 0.0
    return (sim_n - prod_n) / prod_n

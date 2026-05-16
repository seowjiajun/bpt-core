"""Orchestrates a sim-vs-prod comparison across the metrics we have.

Threshold defaults are set conservatively so a vanilla run-vs-run check
flags the obvious divergences (PnL off by 25%+, markout shape KS > 0.10)
without nuisance failures from sampling noise.
"""

from __future__ import annotations

from dataclasses import dataclass, field

from .loader import RunData
from .metrics import (
    DistributionDiff,
    ScalarDiff,
    distribution_diff,
    relative_count_diff,
)


# Markout horizons reported by the backtester; same names appear as fields
# on Trade. Listed in increasing horizon so the report orders naturally.
MARKOUT_HORIZONS = ("50ms", "1s", "5s", "30s")


@dataclass(frozen=True)
class Thresholds:
    """Pass/fail thresholds. All sided as upper bounds.

    Defaults:
      pnl_rel:           |Δ pnl| / |prod_pnl| ≤ 0.25
      fees_rel:          |Δ fees| / prod_fees ≤ 0.20
      fill_count_rel:    |Δ count| / prod_count ≤ 0.10
      markout_ks:        KS per horizon ≤ 0.10
      fill_price_ks:     ≤ 0.05  (prices should be near-identical
                                  if the strategy hits the same levels)
      equity_curve_ks:   ≤ 0.10
    """

    pnl_rel: float = 0.25
    fees_rel: float = 0.20
    fill_count_rel: float = 0.10
    markout_ks: float = 0.10
    fill_price_ks: float = 0.05
    equity_curve_ks: float = 0.10


@dataclass
class MetricResult:
    name: str
    value: float  # the comparable scalar (e.g. KS or rel_diff)
    threshold: float
    passed: bool
    detail: str = ""  # human-readable extra context


@dataclass
class ComparisonReport:
    sim_run_id: str
    prod_run_id: str
    metrics: list[MetricResult] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return all(m.passed for m in self.metrics)

    @property
    def n_failed(self) -> int:
        return sum(1 for m in self.metrics if not m.passed)


def compare(sim: RunData, prod: RunData, thresholds: Thresholds | None = None) -> ComparisonReport:
    th = thresholds or Thresholds()
    rep = ComparisonReport(
        sim_run_id=sim.run_dir.name,
        prod_run_id=prod.run_dir.name,
    )

    # ── Scalars ─────────────────────────────────────────────────────────────
    pnl = ScalarDiff(sim.total_pnl, prod.total_pnl)
    rep.metrics.append(
        MetricResult(
            name="total_pnl",
            value=abs(pnl.rel_diff) if prod.total_pnl != 0 else abs(pnl.abs_diff),
            threshold=th.pnl_rel,
            passed=abs(pnl.rel_diff) <= th.pnl_rel
            if prod.total_pnl != 0
            else abs(pnl.abs_diff) <= 1.0,  # $1 tolerance when prod pnl is zero
            detail=f"sim={sim.total_pnl:+.4f} prod={prod.total_pnl:+.4f} "
            f"abs_diff={pnl.abs_diff:+.4f}",
        )
    )

    fees = ScalarDiff(sim.total_fees, prod.total_fees)
    rep.metrics.append(
        MetricResult(
            name="fees_paid",
            value=abs(fees.rel_diff) if prod.total_fees != 0 else 0.0,
            threshold=th.fees_rel,
            passed=abs(fees.rel_diff) <= th.fees_rel if prod.total_fees != 0 else True,
            detail=f"sim={sim.total_fees:.4f} prod={prod.total_fees:.4f}",
        )
    )

    fill_count_rel = relative_count_diff(sim.total_fills, prod.total_fills)
    rep.metrics.append(
        MetricResult(
            name="fill_count",
            value=abs(fill_count_rel),
            threshold=th.fill_count_rel,
            passed=abs(fill_count_rel) <= th.fill_count_rel,
            detail=f"sim={sim.total_fills} prod={prod.total_fills}",
        )
    )

    # ── Markout distributions ───────────────────────────────────────────────
    for h in MARKOUT_HORIZONS:
        d = distribution_diff(sim.markout_series(h), prod.markout_series(h))
        rep.metrics.append(
            MetricResult(
                name=f"markout_{h}_ks",
                value=d.ks,
                threshold=th.markout_ks,
                passed=d.ks <= th.markout_ks,
                detail=f"sim_mean={d.sim_mean:+.3f}bps prod_mean={d.prod_mean:+.3f}bps "
                f"n=({d.sim_n},{d.prod_n})",
            )
        )

    # ── Fill price distribution ─────────────────────────────────────────────
    fp = distribution_diff(sim.fill_prices(), prod.fill_prices())
    rep.metrics.append(
        MetricResult(
            name="fill_price_ks",
            value=fp.ks,
            threshold=th.fill_price_ks,
            passed=fp.ks <= th.fill_price_ks,
            detail=f"sim_mean={fp.sim_mean:.6f} prod_mean={fp.prod_mean:.6f}",
        )
    )

    # ── Equity curve ────────────────────────────────────────────────────────
    sim_eq = [p.equity for p in sim.equity_curve]
    prod_eq = [p.equity for p in prod.equity_curve]
    if sim_eq or prod_eq:
        eq = distribution_diff(sim_eq, prod_eq)
        rep.metrics.append(
            MetricResult(
                name="equity_curve_ks",
                value=eq.ks,
                threshold=th.equity_curve_ks,
                passed=eq.ks <= th.equity_curve_ks,
                detail=f"sim_mean={eq.sim_mean:.4f} prod_mean={eq.prod_mean:.4f}",
            )
        )

    return rep

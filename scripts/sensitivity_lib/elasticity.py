"""Per-parameter elasticity computation.

For each numeric strategy parameter, we run the strategy three times:
the baseline, baseline × (1 + perturb_pct), baseline × (1 - perturb_pct).
Elasticity = max(|Δpnl|) / |baseline_pnl|. A param with elasticity > 1.0
means a 20% perturbation moved PnL by more than 100% of baseline — a
knife-edge calibration that won't generalise.

This module is pure data: it takes (baseline, plus, minus) PnL triples
and computes the diagnostics. Orchestration and subprocess-spawning
live in sensitivity.py.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class ParamSpec:
    """A single param being perturbed, identified by its dotted path in
    the params TOML."""

    path: list[str]  # e.g. ["bpt-strategy", "strategy", "params", "gamma"]
    baseline: float  # value read from the config
    perturb_pct: float  # e.g. 0.20 for ±20%

    @property
    def name(self) -> str:
        return ".".join(self.path)

    @property
    def plus(self) -> float:
        return self.baseline * (1.0 + self.perturb_pct)

    @property
    def minus(self) -> float:
        return self.baseline * (1.0 - self.perturb_pct)


@dataclass(frozen=True)
class PnLTriple:
    """PnL observed at baseline, +perturb, -perturb."""

    baseline: float
    plus: float
    minus: float


@dataclass(frozen=True)
class ElasticityResult:
    spec: ParamSpec
    pnl: PnLTriple
    elasticity: float  # max |Δ| / max(|baseline|, ε)
    fragile: bool  # elasticity > threshold

    @property
    def worst_drop_pct(self) -> float:
        """Worst PnL drop as a percentage of baseline. Negative means a drop."""
        if self.pnl.baseline == 0:
            return float("inf") if self.pnl.plus < 0 or self.pnl.minus < 0 else 0.0
        worst = min(self.pnl.plus - self.pnl.baseline, self.pnl.minus - self.pnl.baseline)
        return worst / abs(self.pnl.baseline)


def compute_elasticity(
    spec: ParamSpec, pnl: PnLTriple, fragile_threshold: float = 1.0, baseline_floor: float = 1.0
) -> ElasticityResult:
    """`fragile_threshold` is on elasticity — default 1.0 means "perturbation
    moved PnL by at least 100% of baseline". `baseline_floor` is the
    denominator floor for cases where baseline PnL is near zero (avoids
    division blowups; small baseline PnL strategies can still be evaluated).
    """
    delta_plus = pnl.plus - pnl.baseline
    delta_minus = pnl.minus - pnl.baseline
    max_abs_delta = max(abs(delta_plus), abs(delta_minus))
    denom = max(abs(pnl.baseline), baseline_floor)
    elasticity = max_abs_delta / denom
    return ElasticityResult(
        spec=spec,
        pnl=pnl,
        elasticity=elasticity,
        fragile=elasticity > fragile_threshold,
    )

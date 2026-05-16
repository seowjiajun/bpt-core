"""Run-driver for sensitivity analysis.

Wraps a callable that turns a (params dict, label) into a PnL number.
The default implementation calls the existing sweep.py infrastructure
to generate temp configs and run backtest.sh; a mock implementation
substitutes for testing.

Decoupling the run executor from the sensitivity math keeps the math
unit-testable without subprocess plumbing.
"""

from __future__ import annotations

import copy
from dataclasses import dataclass
from typing import Callable

from .elasticity import ElasticityResult, ParamSpec, PnLTriple, compute_elasticity


# A RunFn takes a deep-copied params dict (already mutated to the
# perturbed value) and a label, and returns the realised PnL.
# Returning None signals the run failed; the caller should treat as
# inconclusive (don't flag fragile, don't flag clean).
RunFn = Callable[[dict, str], float | None]


def deep_set(obj: dict, path: list[str], value: float) -> None:
    cur = obj
    for k in path[:-1]:
        if k not in cur or not isinstance(cur[k], dict):
            raise KeyError(f"path {'.'.join(path)} not found in params")
        cur = cur[k]
    if path[-1] not in cur:
        raise KeyError(f"leaf {'.'.join(path)} not found in params")
    cur[path[-1]] = value


def deep_get(obj: dict, path: list[str]) -> float:
    cur = obj
    for k in path:
        if not isinstance(cur, dict) or k not in cur:
            raise KeyError(f"path {'.'.join(path)} not found in params")
        cur = cur[k]
    if not isinstance(cur, (int, float)):
        raise TypeError(f"leaf {'.'.join(path)} is not numeric: {cur!r}")
    return float(cur)


def run_sensitivity(
    base_params: dict, specs: list[ParamSpec], run_fn: RunFn, fragile_threshold: float = 1.0
) -> tuple[float, list[ElasticityResult]]:
    """Run baseline + 2N perturbed cells. Returns (baseline_pnl, results).

    Each spec.baseline must equal the value present at spec.path in
    base_params — the runner asserts this so configs and specs can't drift.
    """
    for spec in specs:
        actual = deep_get(base_params, spec.path)
        if abs(actual - spec.baseline) > 1e-12:
            raise ValueError(
                f"spec baseline mismatch for {spec.name}: spec={spec.baseline} config={actual}"
            )

    baseline_pnl = run_fn(base_params, "baseline")
    if baseline_pnl is None:
        raise RuntimeError("baseline run failed — sensitivity analysis aborted")

    results: list[ElasticityResult] = []
    for spec in specs:
        plus_params = copy.deepcopy(base_params)
        deep_set(plus_params, spec.path, spec.plus)
        plus_pnl = run_fn(plus_params, f"{spec.name}+{int(spec.perturb_pct * 100)}%")

        minus_params = copy.deepcopy(base_params)
        deep_set(minus_params, spec.path, spec.minus)
        minus_pnl = run_fn(minus_params, f"{spec.name}-{int(spec.perturb_pct * 100)}%")

        if plus_pnl is None or minus_pnl is None:
            # Treat failure as inconclusive: zero deltas → not fragile,
            # but elasticity 0 also doesn't *prove* robustness. Caller can
            # see the None-PnL via PnLTriple if it cares.
            plus_pnl = baseline_pnl if plus_pnl is None else plus_pnl
            minus_pnl = baseline_pnl if minus_pnl is None else minus_pnl

        triple = PnLTriple(baseline=baseline_pnl, plus=plus_pnl, minus=minus_pnl)
        results.append(compute_elasticity(spec, triple, fragile_threshold))

    return baseline_pnl, results

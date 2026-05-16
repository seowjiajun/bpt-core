"""Sequential pre-prod gates.

A Gate is a stage that takes a strategy run output and returns a
GateResult — pass/fail plus diagnostic detail. The gate runner executes
gates in order; later gates only run if earlier ones pass (early-exit
saves the cost of expensive sweeps when the baseline is already broken).

Gates currently implemented:
  baseline       single backtest passes basic PnL/Sharpe thresholds
  walk_forward   train/test PnL ratio within tolerance
  sensitivity    no fragile params under ±perturbation

These are the cheapest gates that meaningfully filter strategies. Replay,
paper-vs-predicted, testnet, and capital-limited live are downstream
gates that need the multi-process stack and live-mode plumbing.
"""

from __future__ import annotations

from dataclasses import dataclass, field


@dataclass
class GateResult:
    name: str
    passed: bool
    detail: str
    metrics: dict = field(default_factory=dict)


@dataclass
class GateThresholds:
    """All upper-bound thresholds. Defaults are conservative — every value
    has a clear motivation:

      min_baseline_return_pct: -1.0
        Strategy may lose 1%/day in baseline window. Stricter rules out
        marginal strategies; looser is meaningless.

      max_baseline_drawdown_pct: 5.0
        Daily max DD ≤ 5% of starting capital. Above this, position sizing
        is too aggressive for the strategy's signal.

      min_baseline_fills: 10
        Below 10 fills/day, the markout / win-rate stats are noise.

      walk_forward_drift_pct: 50.0
        Test-window PnL within 50% of train-window PnL. Above means the
        param fit doesn't generalise out-of-sample.

      sensitivity_fragile_threshold: 1.0
        Elasticity (max |Δpnl| / |baseline|) per perturbed param.
        Above 1.0 means ±20% perturbation moved PnL by more than 100%
        of baseline — knife-edge calibration.

      replay_pnl_rel: 0.05
        Replay test: backtest run vs reference run. PnL within 5% (tighter
        than the validate.py default of 25%, since reproduction is
        supposed to be exact modulo intentional randomness).

      replay_markout_ks: 0.05
        Markout distribution KS within 5% (also tighter than validate.py
        default 10% because we're comparing two backtest runs of the
        same params, not a backtest-vs-prod comparison).
    """

    min_baseline_return_pct: float = -1.0
    max_baseline_drawdown_pct: float = 5.0
    min_baseline_fills: int = 10
    walk_forward_drift_pct: float = 50.0
    sensitivity_fragile_threshold: float = 1.0
    replay_pnl_rel: float = 0.05
    replay_markout_ks: float = 0.05


def gate_baseline(summary: dict, th: GateThresholds) -> GateResult:
    return_pct = float(summary.get("return_pct", 0.0))
    max_dd = float(summary.get("max_drawdown_pct", 0.0))
    n_fills = int(summary.get("total_fills", 0))

    fails: list[str] = []
    if return_pct < th.min_baseline_return_pct:
        fails.append(f"return_pct {return_pct:+.3f} < {th.min_baseline_return_pct}")
    if max_dd > th.max_baseline_drawdown_pct:
        fails.append(f"max_drawdown_pct {max_dd:.3f} > {th.max_baseline_drawdown_pct}")
    if n_fills < th.min_baseline_fills:
        fails.append(f"total_fills {n_fills} < {th.min_baseline_fills}")

    return GateResult(
        name="baseline",
        passed=not fails,
        detail="; ".join(fails)
        if fails
        else f"return={return_pct:+.3f}% dd={max_dd:.3f}% fills={n_fills}",
        metrics={
            "return_pct": return_pct,
            "max_drawdown_pct": max_dd,
            "total_fills": n_fills,
        },
    )


def gate_walk_forward(train_pnl: float, test_pnl: float, th: GateThresholds) -> GateResult:
    """Drift = |test - train| / max(|train|, $1). The $1 floor avoids
    the baseline-near-zero blowup for marginal strategies."""
    drift_abs = abs(test_pnl - train_pnl)
    drift_pct = (drift_abs / max(abs(train_pnl), 1.0)) * 100.0

    passed = drift_pct <= th.walk_forward_drift_pct
    return GateResult(
        name="walk_forward",
        passed=passed,
        detail=f"train_pnl={train_pnl:+.3f} test_pnl={test_pnl:+.3f} "
        f"drift={drift_pct:.1f}% (≤{th.walk_forward_drift_pct}% to pass)",
        metrics={
            "train_pnl": train_pnl,
            "test_pnl": test_pnl,
            "drift_pct": drift_pct,
        },
    )


def gate_sensitivity(elasticity_results: list, th: GateThresholds) -> GateResult:
    """`elasticity_results` is a list of ElasticityResult from
    sensitivity_lib.elasticity. We don't import the type here to keep
    this gate library independent of run-orchestration."""
    fragile = [r for r in elasticity_results if r.fragile]
    return GateResult(
        name="sensitivity",
        passed=not fragile,
        detail=(
            f"all {len(elasticity_results)} param(s) robust"
            if not fragile
            else f"{len(fragile)} fragile: "
            + ", ".join(f"{r.spec.name}(elasticity={r.elasticity:.2f})" for r in fragile)
        ),
        metrics={
            "n_params": len(elasticity_results),
            "n_fragile": len(fragile),
            "max_elasticity": max((r.elasticity for r in elasticity_results), default=0.0),
        },
    )


def gate_determinism(determinism_result, th: GateThresholds) -> GateResult:
    """`determinism_result` is a DeterminismResult from preprod_lib.determinism.
    Strict pass: trades.csv + summary.json byte-identical (modulo wallclock).
    No threshold — determinism is binary."""
    return GateResult(
        name="determinism",
        passed=determinism_result.passed,
        detail=determinism_result.detail,
        metrics={
            "trades_match": determinism_result.trades_match,
            "summary_match": determinism_result.summary_match,
        },
    )


def gate_replay(comparison_report, th: GateThresholds) -> GateResult:
    """`comparison_report` is a ComparisonReport from validate_lib.comparison.
    The replay gate uses tighter thresholds than the prod-validation harness
    (default 5% on PnL/markouts vs validate.py's 25%/10%) because we're
    comparing two backtest runs against each other, not backtest-vs-prod."""
    failed = [m for m in comparison_report.metrics if not m.passed]
    return GateResult(
        name="replay",
        passed=not failed,
        detail=(
            f"{len(comparison_report.metrics)} metrics within tolerance"
            if not failed
            else f"{len(failed)} metric(s) diverged: "
            + ", ".join(f"{m.name}={m.value:.4f}" for m in failed)
        ),
        metrics={
            "n_metrics": len(comparison_report.metrics),
            "n_failed": len(failed),
        },
    )


@dataclass
class GateReport:
    strategy_name: str
    results: list[GateResult] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return all(r.passed for r in self.results)

    @property
    def first_failed(self) -> GateResult | None:
        return next((r for r in self.results if not r.passed), None)

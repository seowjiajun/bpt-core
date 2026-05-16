"""Pre-prod gate logic: each gate's pass/fail boundary."""

import pathlib
import sys
from dataclasses import dataclass

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from preprod_lib.gates import (  # noqa: E402
    GateReport,
    GateThresholds,
    gate_baseline,
    gate_sensitivity,
    gate_walk_forward,
)


# ── gate_baseline ─────────────────────────────────────────────────────────────


def test_baseline_passes_clean_strategy():
    summary = {"return_pct": 0.5, "max_drawdown_pct": 1.0, "total_fills": 200}
    r = gate_baseline(summary, GateThresholds())
    assert r.passed


def test_baseline_fails_on_too_negative_return():
    summary = {"return_pct": -2.0, "max_drawdown_pct": 1.0, "total_fills": 200}
    r = gate_baseline(summary, GateThresholds())
    assert not r.passed
    assert "return_pct" in r.detail


def test_baseline_fails_on_excess_drawdown():
    summary = {"return_pct": 0.5, "max_drawdown_pct": 8.0, "total_fills": 200}
    r = gate_baseline(summary, GateThresholds())
    assert not r.passed
    assert "max_drawdown_pct" in r.detail


def test_baseline_fails_on_too_few_fills():
    summary = {"return_pct": 0.5, "max_drawdown_pct": 1.0, "total_fills": 5}
    r = gate_baseline(summary, GateThresholds())
    assert not r.passed
    assert "total_fills" in r.detail


def test_baseline_aggregates_multiple_failures():
    summary = {"return_pct": -3.0, "max_drawdown_pct": 9.0, "total_fills": 3}
    r = gate_baseline(summary, GateThresholds())
    assert not r.passed
    assert "return_pct" in r.detail and "drawdown" in r.detail and "total_fills" in r.detail


# ── gate_walk_forward ─────────────────────────────────────────────────────────


def test_walk_forward_passes_when_test_close_to_train():
    r = gate_walk_forward(train_pnl=-10.0, test_pnl=-12.0, th=GateThresholds())
    # drift = 2.0 / 10.0 = 20% → ≤ 50% → pass
    assert r.passed


def test_walk_forward_fails_on_large_drift():
    r = gate_walk_forward(train_pnl=-10.0, test_pnl=-25.0, th=GateThresholds())
    # drift = 15 / 10 = 150% → > 50% → fail
    assert not r.passed


def test_walk_forward_floors_baseline_to_avoid_zero_blowup():
    # Train PnL is essentially 0; without the floor, drift = inf.
    r = gate_walk_forward(train_pnl=0.5, test_pnl=2.0, th=GateThresholds())
    # drift = 1.5 / max(0.5, 1.0) = 1.5 / 1.0 = 150% → > 50% → fail
    assert not r.passed
    assert "drift" in r.detail


# ── gate_sensitivity ──────────────────────────────────────────────────────────


@dataclass
class FakeSpec:
    name: str


@dataclass
class FakeElasticity:
    spec: FakeSpec
    elasticity: float
    fragile: bool


def test_sensitivity_passes_when_no_fragile_params():
    results = [
        FakeElasticity(FakeSpec("gamma"), 0.10, False),
        FakeElasticity(FakeSpec("kappa"), 0.20, False),
    ]
    r = gate_sensitivity(results, GateThresholds())
    assert r.passed


def test_sensitivity_fails_on_any_fragile_param():
    results = [
        FakeElasticity(FakeSpec("gamma"), 0.10, False),
        FakeElasticity(FakeSpec("kappa"), 1.50, True),
    ]
    r = gate_sensitivity(results, GateThresholds())
    assert not r.passed
    assert "kappa" in r.detail


# ── GateReport rollup ─────────────────────────────────────────────────────────


def test_gate_report_passes_only_when_all_pass():
    rep = GateReport(strategy_name="X")
    rep.results = [
        gate_baseline(
            {"return_pct": 0.5, "max_drawdown_pct": 1.0, "total_fills": 50}, GateThresholds()
        ),
        gate_walk_forward(-10.0, -12.0, GateThresholds()),
    ]
    assert rep.passed
    assert rep.first_failed is None


def test_gate_report_first_failed_returned():
    rep = GateReport(strategy_name="X")
    rep.results = [
        gate_baseline(
            {"return_pct": 0.5, "max_drawdown_pct": 1.0, "total_fills": 50}, GateThresholds()
        ),
        gate_walk_forward(-10.0, -25.0, GateThresholds()),  # fails
        gate_walk_forward(-5.0, -5.5, GateThresholds()),  # would pass
    ]
    assert not rep.passed
    assert rep.first_failed.name == "walk_forward"


# ── gate_determinism ──────────────────────────────────────────────────────────

from preprod_lib.gates import gate_determinism, gate_replay  # noqa: E402


@dataclass
class FakeDeterminism:
    trades_match: bool
    summary_match: bool
    detail: str

    @property
    def passed(self):
        return self.trades_match and self.summary_match


def test_determinism_passes_when_byte_identical():
    r = gate_determinism(FakeDeterminism(True, True, "byte-identical"), GateThresholds())
    assert r.passed


def test_determinism_fails_on_any_diff():
    r = gate_determinism(FakeDeterminism(True, False, "summary.json differs"), GateThresholds())
    assert not r.passed
    assert "summary.json differs" in r.detail


# ── gate_replay ───────────────────────────────────────────────────────────────


@dataclass
class FakeMetric:
    name: str
    value: float
    threshold: float
    passed: bool
    detail: str = ""


@dataclass
class FakeComparison:
    metrics: list


def test_replay_passes_when_all_metrics_within_tolerance():
    cmp = FakeComparison(
        metrics=[
            FakeMetric("total_pnl", 0.01, 0.05, True),
            FakeMetric("markout_50ms_ks", 0.02, 0.05, True),
        ]
    )
    r = gate_replay(cmp, GateThresholds())
    assert r.passed


def test_replay_fails_when_any_metric_exceeds():
    cmp = FakeComparison(
        metrics=[
            FakeMetric("total_pnl", 0.01, 0.05, True),
            FakeMetric("markout_30s_ks", 0.18, 0.05, False),
        ]
    )
    r = gate_replay(cmp, GateThresholds())
    assert not r.passed
    assert "markout_30s_ks" in r.detail

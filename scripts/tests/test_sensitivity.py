"""Sensitivity: elasticity math + run orchestration."""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import pytest  # noqa: E402

from sensitivity_lib.elasticity import (  # noqa: E402
    ParamSpec,
    PnLTriple,
    compute_elasticity,
)
from sensitivity_lib.runner import deep_get, deep_set, run_sensitivity  # noqa: E402


def test_param_spec_perturbation_arithmetic():
    s = ParamSpec(["a", "b"], baseline=10.0, perturb_pct=0.20)
    assert s.plus == 12.0
    assert s.minus == 8.0
    assert s.name == "a.b"


def test_compute_elasticity_robust_param():
    # ±20% perturbation moves PnL by 5% — well below the 100% threshold.
    spec = ParamSpec(["gamma"], baseline=0.05, perturb_pct=0.20)
    pnl = PnLTriple(baseline=-10.0, plus=-10.5, minus=-9.5)
    r = compute_elasticity(spec, pnl, fragile_threshold=1.0)
    assert r.elasticity == 0.05
    assert not r.fragile
    assert r.worst_drop_pct == pytest.approx(-0.05)


def test_compute_elasticity_fragile_param():
    # ±20% perturbation moves PnL by 200% of baseline — knife-edge.
    spec = ParamSpec(["kappa"], baseline=15.0, perturb_pct=0.20)
    pnl = PnLTriple(baseline=-5.0, plus=-15.0, minus=2.0)
    r = compute_elasticity(spec, pnl, fragile_threshold=1.0)
    # max abs delta = max(|−15 − (−5)|, |2 − (−5)|) = max(10, 7) = 10
    # elasticity = 10 / max(|−5|, 1.0) = 10 / 5 = 2.0
    assert r.elasticity == 2.0
    assert r.fragile


def test_compute_elasticity_baseline_floor_protects_near_zero():
    # baseline_pnl ~ 0; without the floor we'd divide by tiny.
    spec = ParamSpec(["x"], 1.0, 0.20)
    pnl = PnLTriple(baseline=0.0001, plus=0.5, minus=-0.5)
    r = compute_elasticity(spec, pnl, fragile_threshold=1.0, baseline_floor=1.0)
    # max abs delta = max(|0.5 - 0.0001|, |-0.5 - 0.0001|) ≈ 0.5001
    # denom = max(|0.0001|, 1.0) = 1.0 → elasticity ≈ 0.5001
    assert r.elasticity == pytest.approx(0.5001, rel=1e-3)
    assert not r.fragile


def test_deep_set_and_get_round_trip():
    cfg = {"a": {"b": {"c": 1.5}}}
    assert deep_get(cfg, ["a", "b", "c"]) == 1.5
    deep_set(cfg, ["a", "b", "c"], 2.0)
    assert cfg["a"]["b"]["c"] == 2.0


def test_deep_set_rejects_missing_path():
    cfg = {"a": {"b": {"c": 1.0}}}
    with pytest.raises(KeyError):
        deep_set(cfg, ["a", "missing", "c"], 99.0)


def test_run_sensitivity_with_mock_runner():
    cfg = {"params": {"alpha": 1.0, "beta": 5.0}}
    specs = [
        ParamSpec(["params", "alpha"], 1.0, 0.10),
        ParamSpec(["params", "beta"], 5.0, 0.10),
    ]

    # PnL function: alpha is robust (small effect), beta is fragile (huge).
    # With baseline PnL = -10, fragile_threshold=1.0 means we need the
    # max |Δ pnl| > 10 to flag fragile. Beta's coefficient produces
    # |Δ pnl| = 15, elasticity = 1.5.
    def fake_pnl(params: dict, label: str) -> float:
        a = params["params"]["alpha"]
        b = params["params"]["beta"]
        return -10.0 - 0.1 * (a - 1.0) / 0.1 - 15.0 * (b - 5.0) / 0.5

    baseline_pnl, results = run_sensitivity(cfg, specs, fake_pnl, fragile_threshold=1.0)
    assert baseline_pnl == -10.0
    by_name = {r.spec.name: r for r in results}
    assert not by_name["params.alpha"].fragile
    assert by_name["params.beta"].fragile


def test_run_sensitivity_baseline_failure_aborts():
    cfg = {"params": {"x": 1.0}}
    specs = [ParamSpec(["params", "x"], 1.0, 0.20)]
    with pytest.raises(RuntimeError):
        run_sensitivity(cfg, specs, lambda p, l: None)


def test_run_sensitivity_validates_baseline_match():
    cfg = {"params": {"x": 2.0}}  # actual is 2.0
    specs = [ParamSpec(["params", "x"], 1.0, 0.20)]  # spec claims 1.0
    with pytest.raises(ValueError):
        run_sensitivity(cfg, specs, lambda p, l: -1.0)


def test_run_sensitivity_partial_failure_continues():
    # If a perturbed run fails (returns None), keep the baseline value
    # for that side and move on — don't abort the whole sweep.
    cfg = {"params": {"x": 1.0}}
    specs = [ParamSpec(["params", "x"], 1.0, 0.20)]
    calls = {"i": 0}

    def flaky(params, label):
        calls["i"] += 1
        if calls["i"] == 2:  # +perturb run fails
            return None
        return -10.0

    baseline_pnl, results = run_sensitivity(cfg, specs, flaky)
    assert baseline_pnl == -10.0
    assert results[0].pnl.plus == -10.0  # filled from baseline
    assert results[0].pnl.minus == -10.0
    assert not results[0].fragile

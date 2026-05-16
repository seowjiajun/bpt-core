#!/usr/bin/env python3
"""
Pre-prod gate runner — qualify a strategy for deployment.

Stages run sequentially; later stages skip if an earlier one fails.
Final exit is 0 if every stage passed, 1 otherwise.

  baseline       single backtest hits PnL/drawdown/fill thresholds
  walk_forward   train/test drift within tolerance (one split)
  sensitivity    no fragile params under ±perturbation

Two execution modes:

1. --from-results
     Reads pre-existing summary.json + sensitivity JSON. Cheap; lets
     you re-evaluate gates with different thresholds without rerunning.

2. (no flag — actually runs)
     Invokes sweep.py + sensitivity.py to produce fresh runs. Needs the
     multi-process stack built; takes minutes per cell.

Usage:
    # Offline (re-evaluate stored runs):
    scripts/preprod_gate.py \\
        --strategy-name AvellanedaStoikov \\
        --from-results \\
            --baseline-summary path/to/summary.json \\
            --walk-forward-train-pnl -8.4 \\
            --walk-forward-test-pnl -10.1 \\
            --sensitivity-json path/to/sensitivity.json \\
        --thresholds /path/to/thresholds.json
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import asdict, fields

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from preprod_lib.determinism import compare_runs  # noqa: E402
from preprod_lib.gates import (  # noqa: E402
    GateReport,
    GateThresholds,
    gate_baseline,
    gate_determinism,
    gate_replay,
    gate_sensitivity,
    gate_walk_forward,
)
from validate_lib.comparison import Thresholds as ValidateThresholds, compare as validate_compare  # noqa: E402
from validate_lib.loader import load_run  # noqa: E402


class FrozenElasticity:
    """Minimal stand-in for sensitivity_lib.ElasticityResult — lets the gate
    runner consume the JSON dump from sensitivity.py --json-out without
    pulling in the full module."""

    def __init__(self, spec_name: str, elasticity: float, fragile: bool):
        class Spec:
            pass

        self.spec = Spec()
        self.spec.name = spec_name
        self.elasticity = elasticity
        self.fragile = fragile


def load_thresholds(path: str | None) -> GateThresholds:
    if not path:
        return GateThresholds()
    raw = json.loads(pathlib.Path(path).read_text())
    valid = {f.name for f in fields(GateThresholds)}
    bad = set(raw) - valid
    if bad:
        raise SystemExit(f"unknown threshold key(s): {sorted(bad)}; valid: {sorted(valid)}")
    return GateThresholds(**raw)


def load_sensitivity_json(path: str) -> list:
    raw = json.loads(pathlib.Path(path).read_text())
    return [
        FrozenElasticity(
            spec_name=r["param"],
            elasticity=float(r["elasticity"]),
            fragile=bool(r["fragile"]),
        )
        for r in raw["results"]
    ]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--strategy-name", required=True)
    ap.add_argument("--thresholds", help="JSON file overriding GateThresholds defaults")
    ap.add_argument("--json-out", help="Write structured GateReport to this path")

    g = ap.add_argument_group("offline mode (--from-results)")
    g.add_argument(
        "--from-results",
        action="store_true",
        help="Read pre-computed run outputs instead of rerunning",
    )
    g.add_argument("--baseline-summary", help="Path to baseline run's summary.json")
    g.add_argument(
        "--walk-forward-train-pnl", type=float, help="train PnL from prior walk-forward run"
    )
    g.add_argument(
        "--walk-forward-test-pnl", type=float, help="test PnL from prior walk-forward run"
    )
    g.add_argument("--sensitivity-json", help="JSON dump from sensitivity.py --json-out")
    g.add_argument(
        "--determinism-runs",
        nargs=2,
        metavar=("RUN_A", "RUN_B"),
        help="Two run dirs from same-seed back-to-back invocations. "
        "Gate passes iff trades.csv + summary.json byte-identical.",
    )
    g.add_argument(
        "--replay-runs",
        nargs=2,
        metavar=("CANDIDATE_RUN", "REFERENCE_RUN"),
        help="Replay gate: candidate run dir vs reference run dir. "
        "Compared via validate.py with tighter thresholds.",
    )

    args = ap.parse_args()
    th = load_thresholds(args.thresholds)

    if not args.from_results:
        raise SystemExit(
            "Live execution mode not yet wired — pass --from-results with the three "
            "pre-computed inputs. Live mode would orchestrate sweep.py + sensitivity.py "
            "directly; deferred until the multi-process stack builds."
        )

    rep = GateReport(strategy_name=args.strategy_name)

    # Stage 1: baseline.
    if not args.baseline_summary:
        raise SystemExit("--from-results needs --baseline-summary")
    summary = json.loads(pathlib.Path(args.baseline_summary).read_text())
    r1 = gate_baseline(summary, th)
    rep.results.append(r1)
    print(f"[1/3] baseline   {'PASS' if r1.passed else 'FAIL'}  {r1.detail}", flush=True)
    if not r1.passed:
        _emit(rep, args.json_out)
        return 1

    # Stage 2: walk-forward.
    if args.walk_forward_train_pnl is None or args.walk_forward_test_pnl is None:
        print("[2/3] walk_forward SKIPPED — train/test PnL not provided", flush=True)
    else:
        r2 = gate_walk_forward(args.walk_forward_train_pnl, args.walk_forward_test_pnl, th)
        rep.results.append(r2)
        print(f"[2/3] walk_forward {'PASS' if r2.passed else 'FAIL'}  {r2.detail}", flush=True)
        if not r2.passed:
            _emit(rep, args.json_out)
            return 1

    # Stage 3: sensitivity.
    if not args.sensitivity_json:
        print("[3/5] sensitivity SKIPPED — no --sensitivity-json provided", flush=True)
    else:
        elasticity_results = load_sensitivity_json(args.sensitivity_json)
        r3 = gate_sensitivity(elasticity_results, th)
        rep.results.append(r3)
        print(f"[3/5] sensitivity {'PASS' if r3.passed else 'FAIL'}  {r3.detail}", flush=True)
        if not r3.passed:
            _emit(rep, args.json_out)
            return 1

    # Stage 4: determinism.
    if not args.determinism_runs:
        print("[4/5] determinism SKIPPED — no --determinism-runs provided", flush=True)
    else:
        det = compare_runs(
            pathlib.Path(args.determinism_runs[0]), pathlib.Path(args.determinism_runs[1])
        )
        r4 = gate_determinism(det, th)
        rep.results.append(r4)
        print(f"[4/5] determinism {'PASS' if r4.passed else 'FAIL'}  {r4.detail}", flush=True)
        if not r4.passed:
            _emit(rep, args.json_out)
            return 1

    # Stage 5: replay.
    if not args.replay_runs:
        print("[5/5] replay SKIPPED — no --replay-runs provided", flush=True)
    else:
        sim_run = load_run(pathlib.Path(args.replay_runs[0]))
        ref_run = load_run(pathlib.Path(args.replay_runs[1]))
        # Tighten validate.py thresholds for replay (same params, same tape).
        replay_th = ValidateThresholds(
            pnl_rel=th.replay_pnl_rel,
            fees_rel=th.replay_pnl_rel,
            fill_count_rel=th.replay_pnl_rel,
            markout_ks=th.replay_markout_ks,
            fill_price_ks=th.replay_markout_ks,
            equity_curve_ks=th.replay_markout_ks,
        )
        comp = validate_compare(sim_run, ref_run, replay_th)
        r5 = gate_replay(comp, th)
        rep.results.append(r5)
        print(f"[5/5] replay      {'PASS' if r5.passed else 'FAIL'}  {r5.detail}", flush=True)

    _emit(rep, args.json_out)
    print()
    if rep.passed:
        print(
            f"VERDICT: {args.strategy_name} cleared all gates — proceed to next stage "
            f"(replay test, paper trading, testnet)"
        )
        return 0
    failed = rep.first_failed
    print(f"VERDICT: {args.strategy_name} failed at stage '{failed.name}' — fix and rerun")
    return 1


def _emit(rep: GateReport, json_out: str | None) -> None:
    if not json_out:
        return
    payload = {
        "strategy_name": rep.strategy_name,
        "passed": rep.passed,
        "results": [asdict(r) for r in rep.results],
    }
    pathlib.Path(json_out).write_text(json.dumps(payload, indent=2, sort_keys=True))


if __name__ == "__main__":
    sys.exit(main())

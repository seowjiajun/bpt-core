#!/usr/bin/env python3
"""
Parameter sensitivity sweep — perturb each named parameter ±perturb_pct
around its baseline value and report PnL elasticity. Strategies with
high elasticity on any param are knife-edge calibrations that won't
generalise; high robustness across all params is a precondition for
deployment.

Different from sweep.py:
  sweep.py        Cartesian grid over user-supplied value lists.
  sensitivity.py  ± perturbation of each param around its baseline,
                  one param at a time, fragility flag per param.

Usage:
    scripts/sensitivity.py \\
        --base bpt-strategy/config/avellaneda_stoikov.backtest.toml \\
        --param strategy.params.gamma \\
        --param strategy.params.kappa \\
        --param strategy.params.max_half_spread_bps \\
        --perturb-pct 20

Exit code: 0 if no fragile params, 1 if any param has elasticity >
threshold (default 1.0).
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
import pathlib
import sys
import tomllib

import tomli_w

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
# Re-use sweep.py's run helpers for actual subprocess execution.
import sweep as sweep_mod  # noqa: E402
from sensitivity_lib.elasticity import ParamSpec  # noqa: E402
from sensitivity_lib.runner import deep_get, run_sensitivity  # noqa: E402


REPO = pathlib.Path(__file__).resolve().parent.parent


def make_run_fn(
    base_path: pathlib.Path,
    params_path: pathlib.Path,
    instance_template: dict,
    refdata_path: pathlib.Path | None,
):
    """Returns a RunFn that writes a temp params/instance config and
    invokes backtest.sh, then reads summary.json's total_pnl. Mirrors
    sweep.py's per-cell loop."""
    counter = {"i": 0}

    def run_fn(params_cfg: dict, label: str) -> float | None:
        counter["i"] += 1
        tag = f"sens-{counter['i']:03d}"

        params_tmp = params_path.parent / f"{params_path.stem.split('.')[0]}.{tag}.toml"
        params_tmp.write_bytes(tomli_w.dumps(params_cfg).encode())

        instance_cfg = copy.deepcopy(instance_template)
        instance_cfg["strategy_config"] = os.path.relpath(params_tmp, base_path.parent)
        instance_tmp = base_path.parent / f"{base_path.stem.split('.')[0]}.{tag}.toml"
        instance_tmp.write_bytes(tomli_w.dumps(instance_cfg).encode())

        try:
            ok = sweep_mod.run_one(instance_tmp, refdata_path)
            if not ok:
                print(f"  ✗ {label}: backtest failed", flush=True)
                return None
            params_hash = hashlib.sha256(instance_tmp.read_bytes()).hexdigest()
            run_dir = sweep_mod.find_run_dir_by_params_hash(params_hash)
            if not run_dir:
                print(f"  ✗ {label}: no result dir", flush=True)
                return None
            summary = sweep_mod.load_summary(run_dir) or {}
            pnl = float(summary.get("total_pnl", 0.0))
            print(f"  ✓ {label}: pnl={pnl:+.4f}", flush=True)
            return pnl
        finally:
            params_tmp.unlink(missing_ok=True)
            instance_tmp.unlink(missing_ok=True)

    return run_fn


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--base", required=True, help="Base strategy instance TOML")
    ap.add_argument(
        "--param",
        action="append",
        default=[],
        help="Dotted param path. Repeat for multi-param sensitivity. "
        "Baseline value is read from the config.",
    )
    ap.add_argument(
        "--perturb-pct", type=float, default=20.0, help="± perturbation percentage. Default 20."
    )
    ap.add_argument(
        "--fragile-threshold",
        type=float,
        default=1.0,
        help="Elasticity above this flags the param as knife-edge. Default 1.0.",
    )
    ap.add_argument(
        "--refdata-config",
        default=str(REPO / "bpt-refdata" / "config" / "bpt-refdata.backtest-hl.toml"),
    )
    ap.add_argument("--json-out", help="Optional JSON dump of the elasticity table")
    args = ap.parse_args()

    if not args.param:
        raise SystemExit("Need at least one --param")

    base_path = pathlib.Path(args.base).resolve()
    instance = tomllib.loads(base_path.read_text())
    params_rel = instance.get("strategy_config")
    if not params_rel:
        raise SystemExit(f"{base_path} has no strategy_config pointer")
    params_path = (base_path.parent / params_rel).resolve()
    base_params = tomllib.loads(params_path.read_text())

    perturb_pct = args.perturb_pct / 100.0
    specs: list[ParamSpec] = []
    for p in args.param:
        path = p.split(".")
        baseline = deep_get(base_params, path)
        specs.append(ParamSpec(path=path, baseline=baseline, perturb_pct=perturb_pct))

    print(
        f"Sensitivity: {len(specs)} param(s) × ±{args.perturb_pct}%  "
        f"({2 * len(specs) + 1} backtest runs)",
        flush=True,
    )
    for s in specs:
        print(
            f"  {s.name}: baseline={s.baseline}  plus={s.plus:.6g}  minus={s.minus:.6g}", flush=True
        )
    print(flush=True)

    refdata_path = pathlib.Path(args.refdata_config).resolve() if args.refdata_config else None
    run_fn = make_run_fn(base_path, params_path, instance, refdata_path)

    baseline_pnl, results = run_sensitivity(
        base_params, specs, run_fn, fragile_threshold=args.fragile_threshold
    )

    print()
    print(f"Baseline PnL: {baseline_pnl:+.4f}")
    print()
    name_w = max(len(r.spec.name) for r in results)
    print(
        f"{'param':<{name_w}}  {'baseline':>10}  {'+pnl':>10}  {'-pnl':>10}  "
        f"{'elasticity':>11}  {'worst_drop%':>11}  status"
    )
    print("-" * (name_w + 70))
    for r in results:
        status = "FRAGILE" if r.fragile else "ok"
        print(
            f"{r.spec.name:<{name_w}}  {r.pnl.baseline:>+10.4f}  {r.pnl.plus:>+10.4f}  "
            f"{r.pnl.minus:>+10.4f}  {r.elasticity:>11.4f}  "
            f"{r.worst_drop_pct * 100:>+11.2f}  {status}"
        )
    print()
    fragile = [r for r in results if r.fragile]
    if fragile:
        print(
            f"VERDICT: FAIL — {len(fragile)} fragile param(s): "
            f"{', '.join(r.spec.name for r in fragile)}"
        )
    else:
        print("VERDICT: PASS — all params robust under perturbation.")

    if args.json_out:
        payload = {
            "baseline_pnl": baseline_pnl,
            "perturb_pct": args.perturb_pct,
            "fragile_threshold": args.fragile_threshold,
            "results": [
                {
                    "param": r.spec.name,
                    "baseline": r.spec.baseline,
                    "plus_value": r.spec.plus,
                    "minus_value": r.spec.minus,
                    "baseline_pnl": r.pnl.baseline,
                    "plus_pnl": r.pnl.plus,
                    "minus_pnl": r.pnl.minus,
                    "elasticity": r.elasticity,
                    "worst_drop_pct": r.worst_drop_pct,
                    "fragile": r.fragile,
                }
                for r in results
            ],
        }
        pathlib.Path(args.json_out).write_text(json.dumps(payload, indent=2))

    return 1 if fragile else 0


if __name__ == "__main__":
    sys.exit(main())

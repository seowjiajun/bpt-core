#!/usr/bin/env python3
"""
Validation harness — compare a backtester run dir against a recorded prod run.

Both inputs must produce the same on-disk schema:
    summary.json    aggregate stats
    trades.csv      one row per fill
    pnl_curve.csv   equity vs time

The backtester writes this layout directly. To validate against a real
production day, a separate extraction tool (TODO) needs to convert the
order-gateway's exec-report log into the same shape.

Exit code: 0 if all metrics pass, 1 if any metric exceeds its threshold.

Usage:
    scripts/validate.py \\
        --sim-dir bpt-backtester/results/AvellanedaStoikov_..._2026-05-07_2026-05-07 \\
        --prod-dir /opt/bpt/data/prod-runs/AvellanedaStoikov_2026-05-07
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from dataclasses import asdict, fields

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from validate_lib.comparison import Thresholds, compare  # noqa: E402
from validate_lib.loader import load_run  # noqa: E402
from validate_lib.report import to_json, to_text  # noqa: E402


def load_thresholds(path: str | None) -> Thresholds:
    if not path:
        return Thresholds()
    raw = json.loads(pathlib.Path(path).read_text())
    valid = {f.name for f in fields(Thresholds)}
    bad = set(raw) - valid
    if bad:
        raise SystemExit(f"unknown threshold key(s): {sorted(bad)} (valid: {sorted(valid)})")
    return Thresholds(**raw)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--sim-dir", required=True, help="Backtester run directory")
    ap.add_argument("--prod-dir", required=True, help="Recorded prod run directory")
    ap.add_argument(
        "--thresholds",
        help="Optional JSON file overriding default thresholds. Keys: "
        "pnl_rel, fees_rel, fill_count_rel, markout_ks, fill_price_ks, "
        "equity_curve_ks. Values are upper-bound floats.",
    )
    ap.add_argument("--json-out", help="Write the JSON report to this path")
    args = ap.parse_args()

    sim = load_run(args.sim_dir)
    prod = load_run(args.prod_dir)
    th = load_thresholds(args.thresholds)
    rep = compare(sim, prod, th)

    print(to_text(rep))

    if args.json_out:
        pathlib.Path(args.json_out).write_text(to_json(rep))

    return 0 if rep.passed else 1


if __name__ == "__main__":
    sys.exit(main())

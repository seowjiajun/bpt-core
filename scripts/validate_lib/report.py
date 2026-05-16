"""Render ComparisonReport as text + JSON."""

from __future__ import annotations

import json
from dataclasses import asdict
from .comparison import ComparisonReport


def to_text(rep: ComparisonReport) -> str:
    lines: list[str] = []
    lines.append(f"== Validation: sim={rep.sim_run_id}")
    lines.append(f"            vs prod={rep.prod_run_id}")
    lines.append("")
    name_w = max(len(m.name) for m in rep.metrics)
    lines.append(f"{'metric':<{name_w}}  {'value':>10}  {'thresh':>8}  status  detail")
    lines.append("-" * (name_w + 2 + 10 + 2 + 8 + 2 + 6 + 2 + 30))
    for m in rep.metrics:
        status = "PASS" if m.passed else "FAIL"
        # If value is "infinite", render the placeholder rather than the
        # gigantic float that float("inf") produces.
        v = "∞" if m.value == float("inf") else f"{m.value:.4f}"
        lines.append(f"{m.name:<{name_w}}  {v:>10}  {m.threshold:>8.4f}  {status:>6}  {m.detail}")
    lines.append("")
    if rep.passed:
        lines.append("VERDICT: pass — backtest matches prod within thresholds.")
    else:
        lines.append(
            f"VERDICT: FAIL — {rep.n_failed} of {len(rep.metrics)} metrics exceeded threshold."
        )
    return "\n".join(lines)


def to_json(rep: ComparisonReport) -> str:
    """Stable JSON form for CI consumption."""
    payload = {
        "sim_run_id": rep.sim_run_id,
        "prod_run_id": rep.prod_run_id,
        "passed": rep.passed,
        "n_failed": rep.n_failed,
        "metrics": [asdict(m) for m in rep.metrics],
    }
    return json.dumps(payload, indent=2, sort_keys=True)

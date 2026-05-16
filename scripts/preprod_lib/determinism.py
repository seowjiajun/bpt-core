"""Determinism gate.

Two runs of the same backtest with the same seed must produce identical
trades.csv + summary.json (modulo `wallclock_duration_ms`, which is the
only field whose value depends on host CPU speed). The SimClock contract
from Phase 2 makes this possible: every strategy-affecting clock read
goes through SimClock, every RNG draw is seeded.

A determinism failure means one of:
  - some strategy code path still reads a wall clock
  - some RNG draw skipped its seed
  - threading reorders inter-stream Aeron arrival before the strategy
  - an unintended I/O dependency (refdata snapshot mtime, etc.)

The gate catches all of them with one byte-comparison.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
from dataclasses import dataclass


# Fields that legitimately differ run-to-run on the same host (CPU speed,
# kernel scheduling). Stripped before hash comparison.
NON_DETERMINISTIC_FIELDS = {"wallclock_duration_ms"}


@dataclass(frozen=True)
class DeterminismResult:
    trades_match: bool
    summary_match: bool
    sim_run_a: pathlib.Path
    sim_run_b: pathlib.Path
    detail: str

    @property
    def passed(self) -> bool:
        return self.trades_match and self.summary_match


def _file_sha256(p: pathlib.Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def _summary_sha256_excluding_nd(p: pathlib.Path) -> str:
    """Hash summary.json after stripping NON_DETERMINISTIC_FIELDS so a
    1ms wallclock blip doesn't false-fail the gate."""
    raw = json.loads(p.read_text())
    for f in NON_DETERMINISTIC_FIELDS:
        raw.pop(f, None)
    canon = json.dumps(raw, sort_keys=True)
    return hashlib.sha256(canon.encode()).hexdigest()


def compare_runs(a: pathlib.Path, b: pathlib.Path) -> DeterminismResult:
    """Compare two run directories for byte-identity (modulo wallclock)."""
    trades_a = a / "trades.csv"
    trades_b = b / "trades.csv"
    summary_a = a / "summary.json"
    summary_b = b / "summary.json"

    missing = [p for p in (trades_a, trades_b, summary_a, summary_b) if not p.exists()]
    if missing:
        return DeterminismResult(
            trades_match=False,
            summary_match=False,
            sim_run_a=a,
            sim_run_b=b,
            detail=f"missing artefacts: {[str(p) for p in missing]}",
        )

    trades_match = _file_sha256(trades_a) == _file_sha256(trades_b)
    summary_match = _summary_sha256_excluding_nd(summary_a) == _summary_sha256_excluding_nd(
        summary_b
    )

    if trades_match and summary_match:
        detail = "byte-identical (excluding wallclock_duration_ms)"
    else:
        flags = []
        if not trades_match:
            flags.append("trades.csv differs")
        if not summary_match:
            flags.append("summary.json differs")
        detail = "; ".join(flags)

    return DeterminismResult(
        trades_match=trades_match,
        summary_match=summary_match,
        sim_run_a=a,
        sim_run_b=b,
        detail=detail,
    )

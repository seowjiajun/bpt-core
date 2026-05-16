"""Determinism gate: byte-compare two run dirs (modulo wallclock)."""

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from preprod_lib.determinism import compare_runs  # noqa: E402


def write_run(d: pathlib.Path, summary: dict, trades_csv: str):
    d.mkdir(parents=True, exist_ok=True)
    (d / "summary.json").write_text(json.dumps(summary))
    (d / "trades.csv").write_text(trades_csv)


def test_identical_runs_pass(tmp_path):
    a = tmp_path / "a"
    b = tmp_path / "b"
    summary = {"total_pnl": -10.0, "total_fills": 5, "wallclock_duration_ms": 1000}
    trades = "ts,price\n100,1.5\n"
    write_run(a, summary, trades)
    write_run(b, summary, trades)

    r = compare_runs(a, b)
    assert r.passed
    assert r.trades_match and r.summary_match


def test_wallclock_difference_does_not_fail(tmp_path):
    """wallclock_duration_ms is the only field allowed to differ."""
    a = tmp_path / "a"
    b = tmp_path / "b"
    summary_base = {"total_pnl": -10.0, "total_fills": 5}
    write_run(a, {**summary_base, "wallclock_duration_ms": 1000}, "ts\n1\n")
    write_run(b, {**summary_base, "wallclock_duration_ms": 1500}, "ts\n1\n")

    r = compare_runs(a, b)
    assert r.passed


def test_pnl_difference_fails(tmp_path):
    a = tmp_path / "a"
    b = tmp_path / "b"
    write_run(a, {"total_pnl": -10.0, "wallclock_duration_ms": 1000}, "ts\n1\n")
    write_run(b, {"total_pnl": -10.5, "wallclock_duration_ms": 1000}, "ts\n1\n")

    r = compare_runs(a, b)
    assert not r.passed
    assert "summary.json differs" in r.detail


def test_trades_difference_fails(tmp_path):
    a = tmp_path / "a"
    b = tmp_path / "b"
    summary = {"total_pnl": -10.0, "wallclock_duration_ms": 1000}
    write_run(a, summary, "ts,price\n100,1.5\n")
    write_run(b, summary, "ts,price\n100,1.6\n")  # diff price

    r = compare_runs(a, b)
    assert not r.passed
    assert "trades.csv differs" in r.detail


def test_missing_files_reported(tmp_path):
    a = tmp_path / "a"
    b = tmp_path / "b"
    a.mkdir()
    b.mkdir()  # both dirs exist but contents missing

    r = compare_runs(a, b)
    assert not r.passed
    assert "missing artefacts" in r.detail

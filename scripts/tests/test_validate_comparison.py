"""End-to-end comparison: synthetic sim+prod run dirs → ComparisonReport."""

import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from validate_lib.comparison import Thresholds, compare  # noqa: E402
from validate_lib.loader import load_run  # noqa: E402


HEADER = (
    "simulation_ts,exchange,symbol,order_id,client_order_id,side,type,"
    "liquidity,qty,price,realized_pnl,fee_paid,equity,"
    "markout_50ms_bps,markout_1s_bps,markout_5s_bps,markout_30s_bps"
)


def write_run(
    d: pathlib.Path,
    *,
    total_pnl: float,
    fees: float,
    trades: list[tuple[float, float, float, float, float, float]],
):
    """trades = list of (price, qty, m50, m1s, m5s, m30s)."""
    d.mkdir(parents=True, exist_ok=True)
    (d / "summary.json").write_text(
        json.dumps(
            {
                "total_pnl": total_pnl,
                "fees_paid_usd": fees,
                "total_fills": len(trades),
            }
        )
    )
    rows = [HEADER]
    for i, (price, qty, m50, m1s, m5s, m30s) in enumerate(trades):
        ts = 1_778_114_000_000_000_000 + i * 1_000_000_000
        rows.append(
            f"{ts},HYPERLIQUID,APE,{i},{i},BUY,LIMIT,MAKER,"
            f"{qty},{price},0,0.001,1000.0,{m50},{m1s},{m5s},{m30s}"
        )
    (d / "trades.csv").write_text("\n".join(rows) + "\n")
    (d / "pnl_curve.csv").write_text("simulation_ts,equity\n0,1000\n")


def test_identical_runs_pass_all_metrics(tmp_path):
    sim = tmp_path / "sim"
    prod = tmp_path / "prod"
    trades = [(0.158 + 0.001 * i, 50.0, -1.5, -1.5, -5.0, -1.0) for i in range(20)]
    for d in (sim, prod):
        write_run(d, total_pnl=-15.0, fees=1.0, trades=trades)

    rep = compare(load_run(sim), load_run(prod))
    assert rep.passed, [m for m in rep.metrics if not m.passed]


def test_pnl_divergence_flagged(tmp_path):
    sim = tmp_path / "sim"
    prod = tmp_path / "prod"
    trades = [(0.158, 50.0, -1.5, -1.5, -5.0, -1.0) for _ in range(20)]
    write_run(sim, total_pnl=-30.0, fees=1.0, trades=trades)
    write_run(prod, total_pnl=-15.0, fees=1.0, trades=trades)

    rep = compare(load_run(sim), load_run(prod))
    assert not rep.passed
    failed = [m.name for m in rep.metrics if not m.passed]
    assert "total_pnl" in failed


def test_markout_distribution_divergence_flagged(tmp_path):
    sim = tmp_path / "sim"
    prod = tmp_path / "prod"
    # sim has consistently worse 30s markouts (more adverse selection)
    sim_trades = [(0.158, 50.0, -1.5, -1.5, -5.0, -10.0) for _ in range(50)]
    prod_trades = [(0.158, 50.0, -1.5, -1.5, -5.0, -1.0) for _ in range(50)]
    write_run(sim, total_pnl=-15.0, fees=1.0, trades=sim_trades)
    write_run(prod, total_pnl=-15.0, fees=1.0, trades=prod_trades)

    rep = compare(load_run(sim), load_run(prod))
    failed = [m.name for m in rep.metrics if not m.passed]
    assert "markout_30s_ks" in failed
    # 50ms / 1s / 5s should pass — same values on both sides.
    assert "markout_50ms_ks" not in failed
    assert "markout_5s_ks" not in failed


def test_fill_count_threshold_respected(tmp_path):
    sim = tmp_path / "sim"
    prod = tmp_path / "prod"
    write_run(sim, total_pnl=-15.0, fees=1.0, trades=[(0.158, 50, -1, -1, -1, -1)] * 100)
    # 9% fewer fills — within default 10% threshold.
    write_run(prod, total_pnl=-15.0, fees=1.0, trades=[(0.158, 50, -1, -1, -1, -1)] * 110)
    rep = compare(load_run(sim), load_run(prod))
    fc = next(m for m in rep.metrics if m.name == "fill_count")
    assert fc.passed, fc.detail


def test_custom_thresholds_apply(tmp_path):
    # Same divergence; tighter threshold flips pass→fail.
    sim = tmp_path / "sim"
    prod = tmp_path / "prod"
    trades = [(0.158, 50.0, -1.5, -1.5, -5.0, -1.0) for _ in range(20)]
    write_run(sim, total_pnl=-16.0, fees=1.0, trades=trades)
    write_run(prod, total_pnl=-15.0, fees=1.0, trades=trades)  # ~6.7% diff

    loose = compare(load_run(sim), load_run(prod), Thresholds(pnl_rel=0.10))
    assert loose.passed

    tight = compare(load_run(sim), load_run(prod), Thresholds(pnl_rel=0.05))
    assert not tight.passed
    assert any(m.name == "total_pnl" for m in tight.metrics if not m.passed)

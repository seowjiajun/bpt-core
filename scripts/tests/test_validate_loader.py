"""Loader: round-trip a synthetic run dir → RunData."""

import json
import pathlib
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

import pytest  # noqa: E402

from validate_lib.loader import load_run  # noqa: E402


TRADES_HEADER = (
    "simulation_ts,exchange,symbol,order_id,client_order_id,side,type,"
    "liquidity,qty,price,realized_pnl,fee_paid,equity,"
    "markout_50ms_bps,markout_1s_bps,markout_5s_bps,markout_30s_bps"
)


def write_run(d: pathlib.Path, *, summary: dict, trades: list[str], pnl_rows: list[str]):
    d.mkdir(parents=True, exist_ok=True)
    (d / "summary.json").write_text(json.dumps(summary))
    (d / "trades.csv").write_text(TRADES_HEADER + "\n" + "\n".join(trades) + "\n")
    (d / "pnl_curve.csv").write_text("simulation_ts,equity\n" + "\n".join(pnl_rows) + "\n")


@pytest.fixture
def synthetic_run_dir(tmp_path):
    write_run(
        tmp_path / "run",
        summary={
            "total_pnl": -15.82,
            "fees_paid_usd": 1.0,
            "total_fills": 2,
            "return_pct": -1.582,
        },
        trades=[
            "1778114578000000000,HYPERLIQUID,APE,1,1,BUY,LIMIT,MAKER,"
            "50,0.158,0,0.001,999.99,-1.5,-1.5,-5.3,-1.2",
            "1778114679000000000,HYPERLIQUID,APE,2,2,SELL,LIMIT,MAKER,"
            "50,0.159,0.001,0.001,1000.00,-0.3,-0.3,-0.9,-5.3",
        ],
        pnl_rows=[
            "0,1000.0",
            "1778114578000000000,999.99",
            "1778114679000000000,1000.00",
        ],
    )
    return tmp_path / "run"


def test_load_run_parses_all_three_artefacts(synthetic_run_dir):
    rd = load_run(synthetic_run_dir)
    assert rd.total_fills == 2
    assert rd.total_pnl == -15.82
    assert rd.total_fees == 1.0
    assert rd.fill_prices() == [0.158, 0.159]
    assert rd.fill_qtys() == [50.0, 50.0]
    assert rd.markout_series("50ms") == [-1.5, -0.3]
    assert rd.markout_series("30s") == [-1.2, -5.3]
    assert len(rd.equity_curve) == 3
    assert rd.equity_curve[0].equity == 1000.0
    assert rd.equity_curve[2].equity == 1000.00


def test_load_run_throws_for_missing_dir(tmp_path):
    with pytest.raises(FileNotFoundError):
        load_run(tmp_path / "does-not-exist")


def test_load_run_handles_partial_artefacts(tmp_path):
    # Only summary.json present.
    d = tmp_path / "run"
    d.mkdir()
    (d / "summary.json").write_text(json.dumps({"total_pnl": -3.14, "fees_paid_usd": 0.5}))
    rd = load_run(d)
    assert rd.total_pnl == -3.14
    assert rd.total_fills == 0
    assert rd.equity_curve == []

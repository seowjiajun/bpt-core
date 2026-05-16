"""Load a backtester or prod run directory into structured Python data.

Both sides of the validation comparison must produce the same on-disk
schema:

    <run_dir>/
        summary.json        # aggregate stats (final_equity, return_pct, …)
        trades.csv          # one row per fill
        pnl_curve.csv       # equity vs time, optional

The backtester writes this layout directly. For prod runs, a thin
extraction tool (TODO) needs to convert the order-gateway's exec-report
log into the same shape — that's how we get an apples-to-apples diff.
"""

from __future__ import annotations

import csv
import json
import pathlib
from dataclasses import dataclass, field


@dataclass
class Trade:
    simulation_ts: int
    exchange: str
    symbol: str
    order_id: str
    side: str  # BUY | SELL
    type: str  # LIMIT | MARKET | POST_ONLY
    liquidity: str  # MAKER | TAKER
    qty: float
    price: float
    realized_pnl: float
    fee_paid: float
    equity: float
    markout_50ms_bps: float
    markout_1s_bps: float
    markout_5s_bps: float
    markout_30s_bps: float


@dataclass
class EquityPoint:
    ts: int
    equity: float


@dataclass
class RunData:
    """All data needed to compare one run against another."""

    run_dir: pathlib.Path
    summary: dict = field(default_factory=dict)
    trades: list[Trade] = field(default_factory=list)
    equity_curve: list[EquityPoint] = field(default_factory=list)

    # Convenience views.
    @property
    def total_fills(self) -> int:
        return len(self.trades)

    @property
    def total_pnl(self) -> float:
        return float(self.summary.get("total_pnl", 0.0))

    @property
    def total_fees(self) -> float:
        return float(self.summary.get("fees_paid_usd", 0.0))

    def markout_series(self, horizon: str) -> list[float]:
        """`horizon` is one of '50ms', '1s', '5s', '30s'."""
        attr = f"markout_{horizon}_bps"
        return [float(getattr(t, attr)) for t in self.trades]

    def fill_prices(self) -> list[float]:
        return [t.price for t in self.trades]

    def fill_qtys(self) -> list[float]:
        return [t.qty for t in self.trades]


def _coerce_int(s: str) -> int:
    try:
        return int(s)
    except ValueError:
        return int(float(s))


def _coerce_float(s: str) -> float:
    try:
        return float(s)
    except ValueError:
        return 0.0


def load_run(run_dir: pathlib.Path | str) -> RunData:
    """Reads the three artefacts in a run dir. Missing files don't throw —
    they leave the corresponding RunData attribute empty so the comparison
    can still cover whatever's available."""
    run_dir = pathlib.Path(run_dir)
    if not run_dir.is_dir():
        raise FileNotFoundError(f"run dir not found: {run_dir}")

    rd = RunData(run_dir=run_dir)

    summary_path = run_dir / "summary.json"
    if summary_path.exists():
        rd.summary = json.loads(summary_path.read_text())

    trades_path = run_dir / "trades.csv"
    if trades_path.exists():
        with trades_path.open() as f:
            reader = csv.DictReader(f)
            for row in reader:
                rd.trades.append(
                    Trade(
                        simulation_ts=_coerce_int(row["simulation_ts"]),
                        exchange=row["exchange"],
                        symbol=row["symbol"],
                        order_id=row["order_id"],
                        side=row["side"],
                        type=row["type"],
                        liquidity=row["liquidity"],
                        qty=_coerce_float(row["qty"]),
                        price=_coerce_float(row["price"]),
                        realized_pnl=_coerce_float(row.get("realized_pnl", "0")),
                        fee_paid=_coerce_float(row.get("fee_paid", "0")),
                        equity=_coerce_float(row.get("equity", "0")),
                        markout_50ms_bps=_coerce_float(row.get("markout_50ms_bps", "0")),
                        markout_1s_bps=_coerce_float(row.get("markout_1s_bps", "0")),
                        markout_5s_bps=_coerce_float(row.get("markout_5s_bps", "0")),
                        markout_30s_bps=_coerce_float(row.get("markout_30s_bps", "0")),
                    )
                )

    pnl_path = run_dir / "pnl_curve.csv"
    if pnl_path.exists():
        with pnl_path.open() as f:
            reader = csv.DictReader(f)
            for row in reader:
                # backtester writes (simulation_ts, equity).
                rd.equity_curve.append(
                    EquityPoint(
                        ts=_coerce_int(row["simulation_ts"]),
                        equity=_coerce_float(row["equity"]),
                    )
                )

    return rd

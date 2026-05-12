from __future__ import annotations

import polars as pl
import pytest


@pytest.fixture
def refdata_df() -> pl.DataFrame:
    return pl.DataFrame(
        [
            {
                "instrument_id": 1,
                "exchange": "OKX",
                "symbol": "BTC-USDT-SWAP",
                "inst_type": "PERP",
                "base_ccy": "BTC",
                "quote_ccy": "USDT",
                "tick_size": 0.1,
                "lot_size": 0.01,
                "min_size": 0.01,
            },
            {
                "instrument_id": 2,
                "exchange": "OKX",
                "symbol": "BTC-USDT",
                "inst_type": "SPOT",
                "base_ccy": "BTC",
                "quote_ccy": "USDT",
                "tick_size": 0.1,
                "lot_size": 0.0001,
                "min_size": 0.0001,
            },
            {
                "instrument_id": 3,
                "exchange": "HYPERLIQUID",
                "symbol": "APE",
                "inst_type": "PERP",
                "base_ccy": "APE",
                "quote_ccy": "USD",
                "tick_size": 0.0001,
                "lot_size": 0.1,
                "min_size": 0.1,
            },
            {
                "instrument_id": 4,
                "exchange": "BINANCE",
                "symbol": "ETH-USDT",
                "inst_type": "SPOT",
                "base_ccy": "ETH",
                "quote_ccy": "USDT",
                "tick_size": 0.01,
                "lot_size": 0.0001,
                "min_size": 0.0001,
            },
            {
                "instrument_id": 5,
                "exchange": "DERIBIT",
                "symbol": "BTC-PERPETUAL",
                "inst_type": "PERP",
                "base_ccy": "BTC",
                "quote_ccy": "USD",
                "tick_size": 0.5,
                "lot_size": 10.0,
                "min_size": 10.0,
            },
        ]
    )


@pytest.fixture
def md_samples_lazy() -> pl.LazyFrame:
    rows = []
    # instrument 1 (BTC SWAP): tight spread, lots of samples
    for i in range(500):
        rows.append(
            {
                "instrument_id": 1,
                "ts_ns": 1_000_000_000 + i,
                "best_bid": 80000.0,
                "best_ask": 80001.0,  # 0.125 bps
                "bid_size": 1.5,
                "ask_size": 1.2,
            }
        )
    # instrument 3 (APE PERP HL): wider spread
    for i in range(300):
        rows.append(
            {
                "instrument_id": 3,
                "ts_ns": 1_000_000_000 + i,
                "best_bid": 1.5000,
                "best_ask": 1.5008,  # ~5.3 bps
                "bid_size": 100.0,
                "ask_size": 80.0,
            }
        )
    # instrument 2 (BTC SPOT): below sample threshold
    for i in range(50):
        rows.append(
            {
                "instrument_id": 2,
                "ts_ns": 1_000_000_000 + i,
                "best_bid": 80000.0,
                "best_ask": 80002.0,
                "bid_size": 0.5,
                "ask_size": 0.5,
            }
        )
    return pl.LazyFrame(rows)


@pytest.fixture
def fills_lazy() -> pl.LazyFrame:
    rows = []
    # instrument 1: many fills, slightly negative markout (adverse selection)
    for i in range(50):
        rows.append(
            {
                "instrument_id": 1,
                "ts_ns": i,
                "side": "BUY",
                "price": 80000.0,
                "qty": 0.01,
                "fee_paid": 0.04,  # ~0.5 bps fee
                "mid_at_fill": 80000.5,
                "mid_30s_after": 80000.0,
                "mid_60s_after": 79998.0,  # adversely selected
            }
        )
    # instrument 3: profitable markouts
    for i in range(40):
        rows.append(
            {
                "instrument_id": 3,
                "ts_ns": i,
                "side": "SELL",
                "price": 1.5008,
                "qty": 100.0,
                "fee_paid": 0.015,  # ~1 bp fee
                "mid_at_fill": 1.5004,
                "mid_30s_after": 1.5000,
                "mid_60s_after": 1.4990,  # mid dropped, we sold high → captured edge
            }
        )
    return pl.LazyFrame(rows)

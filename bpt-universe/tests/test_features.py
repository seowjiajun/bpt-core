from __future__ import annotations

import polars as pl
from bpt_universe import features as feat


def test_md_features_aggregates_per_instrument(md_samples_lazy):
    out = feat.md_features(md_samples_lazy).sort("instrument_id")
    assert set(out.columns) >= {
        "instrument_id",
        "samples",
        "spread_bps_mean",
        "spread_bps_p50",
        "spread_bps_p95",
        "depth_top_mean",
    }
    by_id = {r["instrument_id"]: r for r in out.iter_rows(named=True)}
    # Tight BTC SWAP: ~0.125 bps median.
    assert 0.10 < by_id[1]["spread_bps_p50"] < 0.20
    # Wide APE PERP: ~5.3 bps median.
    assert 5.0 < by_id[3]["spread_bps_p50"] < 5.5
    assert by_id[1]["samples"] == 500


def test_md_features_drops_crossed_spreads():
    bad = pl.LazyFrame(
        [
            {
                "instrument_id": 7,
                "ts_ns": 0,
                "best_bid": 100.0,
                "best_ask": 99.0,
                "bid_size": 1.0,
                "ask_size": 1.0,
            },
            {
                "instrument_id": 7,
                "ts_ns": 1,
                "best_bid": 100.0,
                "best_ask": 100.5,
                "bid_size": 1.0,
                "ask_size": 1.0,
            },
        ]
    )
    out = feat.md_features(bad)
    # Only the non-crossed sample contributes to mean spread.
    row = out.filter(pl.col("instrument_id") == 7).to_dicts()[0]
    assert row["spread_bps_mean"] is not None and row["spread_bps_mean"] > 0


def test_fill_features_computes_capture(fills_lazy):
    out = feat.fill_features(fills_lazy, horizon_s=60).sort("instrument_id")
    by_id = {r["instrument_id"]: r for r in out.iter_rows(named=True)}
    # Inst 1: BUY at 80000 with mid drifting down → negative markout for the buyer.
    assert by_id[1]["markout_bps_mean"] < 0
    assert by_id[1]["realized_capture_bps"] < 0
    # Inst 3: SELL at 1.5008, mid dropped → maker won the edge.
    assert by_id[3]["markout_bps_mean"] > 0
    assert by_id[3]["realized_capture_bps"] > 0


def test_metadata_features_passes_through(refdata_df):
    out = feat.metadata_features(refdata_df)
    assert out.height == refdata_df.height
    assert "tick_size" in out.columns

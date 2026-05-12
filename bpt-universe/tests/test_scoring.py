from __future__ import annotations

import polars as pl
from bpt_universe import features as feat
from bpt_universe import scoring


def test_score_refdata_only(refdata_df):
    """No MD, no fills — score still produces a coherent ranking."""
    cfg = scoring.ScoringConfig(min_samples=0, min_fills=0)
    out = scoring.score(feat.metadata_features(refdata_df), cfg=cfg)
    # BINANCE + DERIBIT are filtered out by venue default.
    venues = set(out["exchange"].to_list())
    assert venues == {"OKX", "HYPERLIQUID"}
    # Score column always present and finite.
    assert "score" in out.columns
    assert out["score"].is_finite().all()


def test_score_with_md_uses_spread(refdata_df, md_samples_lazy):
    md = feat.md_features(md_samples_lazy)
    cfg = scoring.ScoringConfig(min_samples=100, min_fills=0)
    out = scoring.score(feat.metadata_features(refdata_df), md=md, cfg=cfg)
    # APE has wider median spread → higher score under default weights.
    rank = {r["symbol"]: r["score"] for r in out.iter_rows(named=True)}
    assert "APE" in rank
    assert rank["APE"] > rank.get("BTC-USDT-SWAP", -1.0)


def test_min_samples_filter_drops_undertested(refdata_df, md_samples_lazy):
    md = feat.md_features(md_samples_lazy)
    cfg = scoring.ScoringConfig(min_samples=200, min_fills=0)
    out = scoring.score(feat.metadata_features(refdata_df), md=md, cfg=cfg)
    # Inst 2 had 50 samples — below threshold, drops.
    assert 2 not in set(out["instrument_id"].to_list())


def test_capture_floor_filters_unprofitable(refdata_df, md_samples_lazy, fills_lazy):
    md = feat.md_features(md_samples_lazy)
    fills = feat.fill_features(fills_lazy)
    cfg = scoring.ScoringConfig(
        min_samples=100,
        min_fills=20,
        min_realized_capture_bps=0.0,  # only profitable instruments survive
    )
    out = scoring.score(feat.metadata_features(refdata_df), md=md, fills=fills, cfg=cfg)
    # Inst 1 (BTC SWAP) has negative capture → filtered out.
    # Inst 3 (APE PERP HL) is the only one passing samples + fills + capture.
    surviving = set(out["instrument_id"].to_list())
    assert 1 not in surviving
    assert 3 in surviving


def test_score_is_sorted_descending(refdata_df, md_samples_lazy):
    md = feat.md_features(md_samples_lazy)
    cfg = scoring.ScoringConfig(min_samples=0, min_fills=0)
    out = scoring.score(feat.metadata_features(refdata_df), md=md, cfg=cfg)
    scores = out["score"].to_list()
    assert scores == sorted(scores, reverse=True)


def test_filter_by_inst_type(refdata_df):
    cfg = scoring.ScoringConfig(inst_types=("PERP",), min_samples=0, min_fills=0)
    out = scoring.score(feat.metadata_features(refdata_df), cfg=cfg)
    assert set(out["inst_type"].to_list()) == {"PERP"}


def test_missing_features_dont_break_score(refdata_df):
    """ScoringConfig with weights for absent columns must not error."""
    cfg = scoring.ScoringConfig(
        min_samples=0,
        min_fills=0,
        weights={"spread_bps_p50": 1.0, "realized_capture_bps": 1.0},
    )
    # No MD, no fills → both weighted columns absent → all scores collapse to 0.
    out = scoring.score(feat.metadata_features(refdata_df), cfg=cfg)
    assert (pl.Series(out["score"]) == 0.0).all()

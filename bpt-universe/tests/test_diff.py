from __future__ import annotations

from pathlib import Path

import polars as pl
from bpt_universe.diff import proposed_instruments, render_diff


def _candidates() -> pl.DataFrame:
    # Note: column order matches the schema produced by scoring.score(); the
    # diff only reads symbol + inst_type so others are irrelevant here.
    return pl.DataFrame(
        [
            {"symbol": "APE", "inst_type": "PERP", "score": 5.0},
            {"symbol": "BTC-USDT-SWAP", "inst_type": "PERP", "score": 3.0},
            {"symbol": "ETH-USDT-SWAP", "inst_type": "PERP", "score": 1.5},
        ]
    )


def test_proposed_instruments_formats_with_inst_type():
    out = proposed_instruments(_candidates())
    assert out == ["APE:PERP", "BTC-USDT-SWAP:PERP", "ETH-USDT-SWAP:PERP"]


def test_proposed_top_n_trims():
    assert proposed_instruments(_candidates(), top_n=2) == ["APE:PERP", "BTC-USDT-SWAP:PERP"]


def test_render_diff_shows_adds_and_removes(tmp_path: Path):
    cfg_path = tmp_path / "as.toml"
    cfg_path.write_text('instruments = ["BTC/USDT:SPOT", "ETH-USDT-SWAP:PERP"]\n')
    text, to_add, to_remove = render_diff(cfg_path, _candidates())
    assert "+APE:PERP" in text
    assert "+BTC-USDT-SWAP:PERP" in text
    assert "-BTC/USDT:SPOT" in text
    # ETH appears in both — should be context (leading space), not add or remove.
    assert "\n ETH-USDT-SWAP:PERP" in text
    assert "BTC/USDT:SPOT" in to_remove
    assert "APE:PERP" in to_add


def test_render_diff_handles_empty_existing(tmp_path: Path):
    cfg_path = tmp_path / "as.toml"
    cfg_path.write_text("instruments = []\n")
    text, to_add, to_remove = render_diff(cfg_path, _candidates())
    assert to_remove == []
    assert len(to_add) == 3
    for sym in ("APE:PERP", "BTC-USDT-SWAP:PERP", "ETH-USDT-SWAP:PERP"):
        assert f"+{sym}" in text

import json
from pathlib import Path

from bpt_ops.common.schema import ExchangeId
from bpt_ops.jobs.instrument_mapping import emit, reconcile
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument


def _fixture_raws() -> list[RawInstrument]:
    return [
        RawInstrument(ExchangeId.OKX, "BTC-USDT", "BTC", "USDT", "SPOT"),
        RawInstrument(ExchangeId.OKX, "ETH-USDT", "ETH", "USDT", "SPOT"),
        RawInstrument(ExchangeId.BINANCE, "BTCUSDT", "BTC", "USDT", "SPOT"),
    ]


def test_emit_writes_one_file_per_present_exchange(tmp_path: Path):
    mapping = reconcile.build(_fixture_raws(), now_ms=1745000000000)
    written = emit.write_per_exchange(mapping, tmp_path)

    names = sorted(p.name for p in written)
    assert names == ["instrument_mapping.binance.json", "instrument_mapping.okx.json"]


def test_emit_produces_deterministic_json(tmp_path: Path):
    mapping = reconcile.build(_fixture_raws(), now_ms=1745000000000)

    first_dir = tmp_path / "run1"
    second_dir = tmp_path / "run2"
    emit.write_per_exchange(mapping, first_dir)
    emit.write_per_exchange(mapping, second_dir)

    for name in ("instrument_mapping.okx.json", "instrument_mapping.binance.json"):
        a = (first_dir / name).read_text()
        b = (second_dir / name).read_text()
        assert a == b, f"{name} is not byte-identical between runs"


def test_emit_okx_file_matches_bpt_refdata_schema(tmp_path: Path):
    """Golden-schema check: the shape must be what bpt-refdata's loader parses."""
    mapping = reconcile.build(_fixture_raws(), now_ms=1745000000000)
    emit.write_per_exchange(mapping, tmp_path)

    parsed = json.loads((tmp_path / "instrument_mapping.okx.json").read_text())

    assert set(parsed.keys()) == {"forward", "reverse", "exported_at", "instrument_count"}
    # forward keys: only OKX (exchange id 2)
    assert all(k.startswith("2_") for k in parsed["forward"])
    # reverse entries carry base/quote/type/exchanges
    for entry in parsed["reverse"].values():
        assert set(entry.keys()) == {"base", "quote", "type", "exchanges"}
        assert list(entry["exchanges"].keys()) == ["2"]  # scoped to OKX only
    assert parsed["instrument_count"] == len(parsed["reverse"]) == 2
    assert parsed["exported_at"] == 1745000000000

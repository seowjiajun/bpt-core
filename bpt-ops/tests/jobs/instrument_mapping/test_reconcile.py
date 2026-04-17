from bpt_ops.common.schema import ExchangeId
from bpt_ops.jobs.instrument_mapping import reconcile
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument


def _okx_btc_spot() -> RawInstrument:
    return RawInstrument(
        exchange=ExchangeId.OKX,
        venue_symbol="BTC-USDT",
        base="BTC",
        quote="USDT",
        instrument_type="SPOT",
    )


def _binance_btc_spot() -> RawInstrument:
    return RawInstrument(
        exchange=ExchangeId.BINANCE,
        venue_symbol="BTCUSDT",
        base="BTC",
        quote="USDT",
        instrument_type="SPOT",
    )


def test_canonical_id_is_stable():
    a = reconcile.canonical_id("BTC", "USDT", "SPOT")
    b = reconcile.canonical_id("BTC", "USDT", "SPOT")
    assert a == b
    assert 0 <= a < 2**32


def test_canonical_id_differs_per_instrument():
    spot = reconcile.canonical_id("BTC", "USDT", "SPOT")
    perp = reconcile.canonical_id("BTC", "USDT", "PERP")
    assert spot != perp


def test_build_merges_exchanges_under_same_canonical_id():
    mapping = reconcile.build([_okx_btc_spot(), _binance_btc_spot()], now_ms=1745000000000)

    assert mapping.instrument_count == 1
    assert len(mapping.reverse) == 1

    cid_str = next(iter(mapping.reverse))
    entry = mapping.reverse[cid_str]
    assert entry.base == "BTC"
    assert entry.quote == "USDT"
    assert entry.type == "SPOT"
    assert entry.exchanges == {"1": "BTCUSDT", "2": "BTC-USDT"}

    assert mapping.forward[f"{ExchangeId.OKX.value}_BTC-USDT"] == int(cid_str)
    assert mapping.forward[f"{ExchangeId.BINANCE.value}_BTCUSDT"] == int(cid_str)

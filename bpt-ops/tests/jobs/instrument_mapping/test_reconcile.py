from bpt_ops.common.schema import ExchangeId
from bpt_ops.jobs.instrument_mapping import reconcile
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument


def _okx_btc_spot() -> RawInstrument:
    return RawInstrument(ExchangeId.OKX, "BTC-USDT", "BTC", "USDT", "SPOT")


def _okx_xrp_spot() -> RawInstrument:
    return RawInstrument(ExchangeId.OKX, "XRP-USDT", "XRP", "USDT", "SPOT")


def _binance_btc_spot() -> RawInstrument:
    return RawInstrument(ExchangeId.BINANCE, "BTCUSDT", "BTC", "USDT", "SPOT")


def test_build_seeds_major_pairs_even_with_no_fetched_data():
    m = reconcile.build([], now_ms=1745000000000)
    # 3 PERP seeds + 3 SPOT seeds
    assert m.instrument_count == 6
    assert "1001" in m.reverse  # BTC PERP
    assert "2001" in m.reverse  # BTC SPOT


def test_build_reuses_seed_id_for_btc_spot():
    # OKX fetches BTC-USDT spot; should land on the seeded id 2001, not a new one
    m = reconcile.build([_okx_btc_spot()], now_ms=1745000000000)
    assert m.forward[f"{int(ExchangeId.OKX)}_BTC-USDT"] == 2001


def test_build_allocates_next_id_for_non_seed_instrument():
    # XRP-USDT SPOT is not seeded, so should get id 2004 (next after 2001/2002/2003)
    m = reconcile.build([_okx_xrp_spot()], now_ms=1745000000000)
    xrp_id = m.forward[f"{int(ExchangeId.OKX)}_XRP-USDT"]
    assert xrp_id == 2004
    assert m.reverse[str(xrp_id)].base == "XRP"


def test_build_unions_exchanges_on_same_canonical_id():
    m = reconcile.build([_okx_btc_spot(), _binance_btc_spot()], now_ms=1745000000000)

    entry = m.reverse["2001"]
    assert entry.exchanges[str(int(ExchangeId.OKX))] == "BTC-USDT"
    assert entry.exchanges[str(int(ExchangeId.BINANCE))] == "BTCUSDT"

    # Forward keys present for both venues
    assert m.forward[f"{int(ExchangeId.OKX)}_BTC-USDT"] == 2001
    assert m.forward[f"{int(ExchangeId.BINANCE)}_BTCUSDT_SPOT"] == 2001


def test_instrument_count_matches_reverse():
    m = reconcile.build([_okx_btc_spot(), _okx_xrp_spot()], now_ms=1745000000000)
    assert m.instrument_count == len(m.reverse)

from bpt_ops.common.schema import ExchangeId, InstrumentMapping
from bpt_ops.jobs.instrument_mapping import seed


def _empty() -> InstrumentMapping:
    return InstrumentMapping(forward={}, reverse={}, exported_at=0, instrument_count=0)


def test_seeds_apply_to_empty_mapping():
    m = seed.apply_seeds(_empty())

    # BTC/ETH/SOL perps pinned at 1001/1002/1003
    for cid, base in [(1001, "BTC"), (1002, "ETH"), (1003, "SOL")]:
        entry = m.reverse[str(cid)]
        assert entry.base == base
        assert entry.quote == "USDT"
        assert entry.type == "PERP"
        # all three PERP venues seeded
        assert int(ExchangeId.BINANCE) in [int(k) for k in entry.exchanges]
        assert int(ExchangeId.OKX) in [int(k) for k in entry.exchanges]
        assert int(ExchangeId.HYPERLIQUID) in [int(k) for k in entry.exchanges]

    # BTC/ETH/SOL spots pinned at 2001/2002/2003
    for cid, base in [(2001, "BTC"), (2002, "ETH"), (2003, "SOL")]:
        entry = m.reverse[str(cid)]
        assert entry.type == "SPOT"
        assert entry.base == base


def test_seeds_are_idempotent():
    m = seed.apply_seeds(_empty())
    m_again = seed.apply_seeds(m)

    assert m.reverse.keys() == m_again.reverse.keys()
    assert m.forward == m_again.forward


def test_binance_spot_forward_key_has_suffix():
    m = seed.apply_seeds(_empty())
    # Binance SPOT BTCUSDT: forward key is "1_BTCUSDT_SPOT"
    assert m.forward[f"{int(ExchangeId.BINANCE)}_BTCUSDT_SPOT"] == 2001
    # Binance PERP BTCUSDT: forward key is "1_BTCUSDT" (no suffix)
    assert m.forward[f"{int(ExchangeId.BINANCE)}_BTCUSDT"] == 1001


def test_okx_forward_keys_have_no_suffix():
    m = seed.apply_seeds(_empty())
    # OKX uses distinct tickers, so SPOT has no suffix
    assert m.forward[f"{int(ExchangeId.OKX)}_BTC-USDT"] == 2001
    assert m.forward[f"{int(ExchangeId.OKX)}_BTC-USDT-SWAP"] == 1001

"""Forward-key construction, with the Binance SPOT/PERP quirk handled.

Binance uses the same ticker (e.g. `BTCUSDT`) for both SPOT and PERP markets,
so a naive key like "<exchange_id>_<symbol>" would collide between the two.
We disambiguate by appending _SPOT to Binance SPOT rows. All other exchanges
(OKX, Hyperliquid, Deribit) have distinct symbols per market and don't need
the suffix.
"""

from __future__ import annotations

from bpt_ops.common.schema import ExchangeId


def build_forward_key(exchange: ExchangeId, venue_symbol: str, instrument_type: str) -> str:
    if exchange == ExchangeId.BINANCE and instrument_type == "SPOT":
        return f"{int(exchange)}_{venue_symbol}_SPOT"
    return f"{int(exchange)}_{venue_symbol}"

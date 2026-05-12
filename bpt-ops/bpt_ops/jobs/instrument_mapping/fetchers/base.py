"""Per-exchange fetcher contract.

Each exchange's adapter implements a `fetch()` that returns a list of
RawInstrument records. The reconcile step turns those into canonical IDs
plus the (forward, reverse) index shape that bpt-refdata consumes.
"""

from __future__ import annotations

from dataclasses import dataclass

from bpt_ops.common.schema import ExchangeId


@dataclass(frozen=True, slots=True)
class RawInstrument:
    exchange: ExchangeId
    venue_symbol: str  # exactly as the exchange returns it (e.g. "BTC-USDT-SWAP")
    base: str  # "BTC"
    quote: str  # "USDT"
    instrument_type: str  # SPOT | PERP | FUTURE | OPTION


def fetch_for(exchange: ExchangeId) -> list[RawInstrument]:
    """Dispatch to the per-exchange fetcher. Keeps the reconcile step venue-agnostic."""
    from bpt_ops.jobs.instrument_mapping.fetchers import binance, deribit, hyperliquid, okx

    match exchange:
        case ExchangeId.OKX:
            return okx.fetch()
        case ExchangeId.BINANCE:
            return binance.fetch()
        case ExchangeId.HYPERLIQUID:
            return hyperliquid.fetch()
        case ExchangeId.DERIBIT:
            return deribit.fetch()

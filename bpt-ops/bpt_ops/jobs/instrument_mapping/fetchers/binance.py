"""Binance instrument fetcher — TODO stub."""

from __future__ import annotations

from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument


def fetch() -> list[RawInstrument]:
    # TODO: hit https://api.binance.com/api/v3/exchangeInfo (SPOT)
    # and https://fapi.binance.com/fapi/v1/exchangeInfo (USDT-M PERP).
    raise NotImplementedError("Binance fetcher not yet implemented")

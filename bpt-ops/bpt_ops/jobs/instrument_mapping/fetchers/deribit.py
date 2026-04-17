"""Deribit instrument fetcher — TODO stub."""
from __future__ import annotations

from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument


def fetch() -> list[RawInstrument]:
    # TODO: https://www.deribit.com/api/v2/public/get_instruments
    # across currencies BTC, ETH (filter by expired=false).
    raise NotImplementedError("Deribit fetcher not yet implemented")

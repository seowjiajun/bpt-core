"""Hyperliquid instrument fetcher — TODO stub."""
from __future__ import annotations

from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument


def fetch() -> list[RawInstrument]:
    # TODO: POST https://api.hyperliquid.xyz/info with {"type": "meta"}.
    raise NotImplementedError("Hyperliquid fetcher not yet implemented")

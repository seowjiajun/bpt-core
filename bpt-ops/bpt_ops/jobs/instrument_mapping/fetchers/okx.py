"""OKX instrument fetcher.

Queries /api/v5/public/instruments for SPOT + SWAP (linear USDT only).
Filters to live state and USDT quote/settle to keep the canonical universe
manageable (matches bpt/data-forge's production behaviour).
"""

from __future__ import annotations

from bpt_ops.common.http import get_json
from bpt_ops.common.schema import ExchangeId
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument

_BASE_URL = "https://www.okx.com/api/v5/public/instruments"


def fetch() -> list[RawInstrument]:
    return _fetch_spot() + _fetch_perp()


def _fetch_spot() -> list[RawInstrument]:
    resp = get_json(_BASE_URL, params={"instType": "SPOT"})
    if resp.get("code") != "0":
        raise RuntimeError(f"OKX SPOT fetch failed: {resp.get('msg')}")

    out: list[RawInstrument] = []
    for row in resp.get("data", []):
        if row.get("state") != "live":
            continue
        if row.get("quoteCcy") != "USDT":
            continue
        # instId format: BASE-QUOTE (e.g. BTC-USDT)
        base = row["instId"].split("-", 1)[0].upper()
        out.append(
            RawInstrument(
                exchange=ExchangeId.OKX,
                venue_symbol=row["instId"],
                base=base,
                quote="USDT",
                instrument_type="SPOT",
            )
        )
    return out


def _fetch_perp() -> list[RawInstrument]:
    resp = get_json(_BASE_URL, params={"instType": "SWAP"})
    if resp.get("code") != "0":
        raise RuntimeError(f"OKX SWAP fetch failed: {resp.get('msg')}")

    out: list[RawInstrument] = []
    for row in resp.get("data", []):
        if row.get("state") != "live":
            continue
        if row.get("ctType") != "linear":
            continue
        if row.get("settleCcy") != "USDT":
            continue
        # instId format: BASE-QUOTE-SWAP (e.g. BTC-USDT-SWAP)
        base = row["instId"].split("-", 1)[0].upper()
        out.append(
            RawInstrument(
                exchange=ExchangeId.OKX,
                venue_symbol=row["instId"],
                base=base,
                quote="USDT",
                instrument_type="PERP",
            )
        )
    return out

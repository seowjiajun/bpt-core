"""OKX instrument fetcher.

Queries the public /api/v5/public/instruments endpoint for each instType we
care about (SPOT, SWAP). No auth needed for these. Returns a flat list of
RawInstrument records.
"""
from __future__ import annotations

from bpt_ops.common.http import get_json
from bpt_ops.common.schema import ExchangeId
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument

_BASE_URL = "https://www.okx.com/api/v5/public/instruments"


def fetch() -> list[RawInstrument]:
    out: list[RawInstrument] = []
    for inst_type, normalized in (("SPOT", "SPOT"), ("SWAP", "PERP")):
        resp = get_json(_BASE_URL, params={"instType": inst_type})
        if resp.get("code") != "0":
            raise RuntimeError(f"OKX {inst_type} fetch failed: {resp.get('msg')}")
        for row in resp.get("data", []):
            out.append(
                RawInstrument(
                    exchange=ExchangeId.OKX,
                    venue_symbol=row["instId"],
                    base=row["baseCcy"] or row.get("ctValCcy", ""),
                    quote=row["quoteCcy"] or row.get("settleCcy", ""),
                    instrument_type=normalized,
                )
            )
    return out

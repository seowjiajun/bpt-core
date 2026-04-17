"""Assign canonical IDs and build the InstrumentMapping.

Canonical ID policy: deterministic hash of (base, quote, type). Using a hash
rather than a monotonic counter means adding/removing an instrument doesn't
renumber every other one — a re-run produces stable IDs, which matters because
order-gateway and strategy persist canonical IDs in memory and Aeron messages.
"""
from __future__ import annotations

import hashlib
import time
from collections.abc import Iterable

from bpt_ops.common.schema import InstrumentMapping, ReverseEntry
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument


def canonical_id(base: str, quote: str, inst_type: str) -> int:
    """Stable 32-bit ID derived from (base, quote, type). Collisions extremely unlikely at N<10k."""
    h = hashlib.blake2b(f"{base}|{quote}|{inst_type}".encode(), digest_size=4).digest()
    return int.from_bytes(h, "big")


def build(raws: Iterable[RawInstrument], *, now_ms: int | None = None) -> InstrumentMapping:
    """Turn per-exchange RawInstrument lists into the merged forward/reverse mapping."""
    forward: dict[str, int] = {}
    reverse: dict[str, ReverseEntry] = {}

    for r in raws:
        cid = canonical_id(r.base, r.quote, r.instrument_type)
        forward[f"{int(r.exchange)}_{r.venue_symbol}"] = cid

        entry = reverse.get(str(cid))
        if entry is None:
            reverse[str(cid)] = ReverseEntry(
                base=r.base,
                quote=r.quote,
                type=r.instrument_type,
                exchanges={str(int(r.exchange)): r.venue_symbol},
            )
        else:
            # Different venue, same canonical instrument — union exchanges dict.
            entry.exchanges[str(int(r.exchange))] = r.venue_symbol

    return InstrumentMapping(
        forward=forward,
        reverse=reverse,
        exported_at=now_ms if now_ms is not None else int(time.time() * 1000),
        instrument_count=len(reverse),
    )

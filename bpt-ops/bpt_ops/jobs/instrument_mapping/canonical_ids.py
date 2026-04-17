"""Canonical-ID allocator.

IDs are bucketed by instrument type so you can eyeball an ID and know the
asset class. Within each bucket, new IDs are `max_existing + 1` — stable
across runs because a round-trip always loads the current mapping first.

This matches the pattern already in production in bpt/data-forge; the intent
is that canonical_ids never get renumbered once assigned. Once a strategy has
internalised id=2003 as SOL-USDT SPOT, that must stay true forever.
"""
from __future__ import annotations

from bpt_ops.common.schema import InstrumentMapping, ReverseEntry

# Inclusive ranges. Leave room to expand; cross a boundary by adding a new type.
_RANGE_START: dict[str, int] = {"PERP": 1001, "SPOT": 2001, "FUTURES": 3001}
_RANGE_END: dict[str, int] = {"PERP": 1999, "SPOT": 2999, "FUTURES": 3999}


def next_canonical_id(mapping: InstrumentMapping, instrument_type: str) -> int:
    """Return the next available canonical ID for an instrument type."""
    start = _RANGE_START[instrument_type]
    end = _RANGE_END[instrument_type]

    existing = [
        int(cid)
        for cid, entry in mapping.reverse.items()
        if entry.type == instrument_type and start <= int(cid) <= end
    ]
    if not existing:
        return start
    nxt = max(existing) + 1
    if nxt > end:
        raise RuntimeError(f"{instrument_type} canonical-id range exhausted (ceiling {end})")
    return nxt


def find_canonical_id(
    mapping: InstrumentMapping, base: str, quote: str, instrument_type: str
) -> int | None:
    """Return the canonical ID for a base/quote/type triple, or None."""
    for cid_str, entry in mapping.reverse.items():
        if entry.base == base and entry.quote == quote and entry.type == instrument_type:
            return int(cid_str)
    return None


def assign_canonical_id(
    mapping: InstrumentMapping, base: str, quote: str, instrument_type: str
) -> int:
    """Reuse an existing ID if the (base, quote, type) triple is already known,
    otherwise allocate the next one in the type's range."""
    existing = find_canonical_id(mapping, base, quote, instrument_type)
    if existing is not None:
        return existing
    return next_canonical_id(mapping, instrument_type)


def ensure_reverse_entry(
    mapping: InstrumentMapping,
    canonical_id: int,
    base: str,
    quote: str,
    instrument_type: str,
) -> ReverseEntry:
    """Return the reverse entry for a canonical_id, creating it if missing."""
    cid_str = str(canonical_id)
    if cid_str not in mapping.reverse:
        mapping.reverse[cid_str] = ReverseEntry(
            base=base, quote=quote, type=instrument_type, exchanges={}
        )
    return mapping.reverse[cid_str]

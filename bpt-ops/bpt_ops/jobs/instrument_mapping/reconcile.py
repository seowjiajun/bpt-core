"""Turn per-exchange RawInstrument lists into a canonical InstrumentMapping.

Pipeline:
  1. Start from an empty mapping (or one loaded from the previous run, if a
     round-trip is ever wired in).
  2. Apply seeds so major pairs lock onto their reserved IDs.
  3. For each fetched instrument, assign (or reuse) a canonical ID, union
     its exchange symbol into the reverse entry, add the forward key.
"""
from __future__ import annotations

import time
from collections.abc import Iterable

from bpt_ops.common.schema import InstrumentMapping
from bpt_ops.jobs.instrument_mapping.canonical_ids import (
    assign_canonical_id,
    ensure_reverse_entry,
)
from bpt_ops.jobs.instrument_mapping.fetchers.base import RawInstrument
from bpt_ops.jobs.instrument_mapping.forward_key import build_forward_key
from bpt_ops.jobs.instrument_mapping.seed import apply_seeds


def build(
    raws: Iterable[RawInstrument],
    *,
    now_ms: int | None = None,
    prev: InstrumentMapping | None = None,
) -> InstrumentMapping:
    """Produce the merged mapping. If `prev` is given, its canonical IDs are
    preserved — new instruments append past the highest in-range ID."""
    mapping = (
        prev.model_copy(deep=True)
        if prev is not None
        else InstrumentMapping(forward={}, reverse={}, exported_at=0, instrument_count=0)
    )
    apply_seeds(mapping)

    for r in raws:
        cid = assign_canonical_id(mapping, r.base, r.quote, r.instrument_type)
        entry = ensure_reverse_entry(mapping, cid, r.base, r.quote, r.instrument_type)
        ex_str = str(int(r.exchange))
        entry.exchanges[ex_str] = r.venue_symbol

        fwd_key = build_forward_key(r.exchange, r.venue_symbol, r.instrument_type)
        mapping.forward[fwd_key] = cid

    mapping.exported_at = now_ms if now_ms is not None else int(time.time() * 1000)
    mapping.instrument_count = len(mapping.reverse)
    return mapping

"""Write per-exchange mapping files atomically.

Each file ships to bpt-refdata under config/generated/instrument_mapping.<exchange>.json;
refdata's InstrumentMappingMerger consumes them at service startup.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

from bpt_ops.common.schema import ExchangeId, InstrumentMapping


def _filter_to(mapping: InstrumentMapping, exchange: ExchangeId) -> InstrumentMapping:
    """Produce a mapping scoped to one exchange (only rows that reference it)."""
    ex_str = str(int(exchange))
    forward = {k: v for k, v in mapping.forward.items() if k.startswith(f"{ex_str}_")}

    reverse: dict[str, object] = {}
    for cid_str, entry in mapping.reverse.items():
        if ex_str in entry.exchanges:
            reverse[cid_str] = entry.model_copy(
                update={"exchanges": {ex_str: entry.exchanges[ex_str]}}
            )

    return InstrumentMapping(
        forward=forward,
        reverse=reverse,  # type: ignore[arg-type]
        exported_at=mapping.exported_at,
        instrument_count=len(reverse),
    )


def write_per_exchange(mapping: InstrumentMapping, out_dir: Path) -> list[Path]:
    """Write one instrument_mapping.<exchange>.json file per exchange that appears in `mapping`.

    Idempotent: if the existing file on disk is identical modulo the
    `exported_at` timestamp, we preserve the existing timestamp so the
    on-disk bytes don't change. This prevents the daily ops cron from
    opening a PR just because the wall clock moved — PRs only appear on
    a real content change.

    Each scoped file is re-validated through InstrumentMapping (pydantic)
    before hitting disk. Writes via .tmp + rename so a partial write can't
    corrupt the target.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    present: set[ExchangeId] = set()
    for key in mapping.forward:
        ex_id = int(key.split("_", 1)[0])
        present.add(ExchangeId(ex_id))

    for ex in sorted(present, key=lambda e: e.value):
        scoped = _filter_to(mapping, ex)
        InstrumentMapping.model_validate(scoped.model_dump())

        target = out_dir / f"instrument_mapping.{ex.name.lower()}.json"

        # If the existing file's content (sans exported_at) equals the new
        # content (sans exported_at), preserve the existing timestamp so
        # the output is byte-identical. Only advance time on real changes.
        if target.exists():
            existing = json.loads(target.read_text())
            scoped_dict = scoped.model_dump()
            if _semantic_equal(existing, scoped_dict):
                scoped = scoped.model_copy(update={"exported_at": existing["exported_at"]})

        payload = _serialise(scoped)
        tmp = target.with_suffix(target.suffix + ".tmp")
        tmp.write_text(payload)
        os.replace(tmp, target)
        written.append(target)

    return written


def _semantic_equal(a: dict, b: dict) -> bool:
    """Compare two mapping dicts ignoring the exported_at timestamp."""
    return {k: v for k, v in a.items() if k != "exported_at"} == {
        k: v for k, v in b.items() if k != "exported_at"
    }


def _serialise(mapping: InstrumentMapping) -> str:
    """JSON-serialise with sorted keys + trailing newline for stable diffs."""
    d = mapping.model_dump()
    d["forward"] = dict(sorted(d["forward"].items()))
    d["reverse"] = dict(sorted(d["reverse"].items(), key=lambda kv: int(kv[0])))
    for entry in d["reverse"].values():
        entry["exchanges"] = dict(sorted(entry["exchanges"].items(), key=lambda kv: int(kv[0])))
    return json.dumps(d, indent=2) + "\n"

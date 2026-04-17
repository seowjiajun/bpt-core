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

    Keys in reverse are sorted for deterministic output (helps git diffs). Writes via
    .tmp + rename so a partial write can't corrupt the target file.
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []

    present: set[ExchangeId] = set()
    for key in mapping.forward:
        ex_id = int(key.split("_", 1)[0])
        present.add(ExchangeId(ex_id))

    for ex in sorted(present, key=lambda e: e.value):
        scoped = _filter_to(mapping, ex)
        payload = _serialise(scoped)

        target = out_dir / f"instrument_mapping.{ex.name.lower()}.json"
        tmp = target.with_suffix(target.suffix + ".tmp")
        tmp.write_text(payload)
        os.replace(tmp, target)
        written.append(target)

    return written


def _serialise(mapping: InstrumentMapping) -> str:
    """JSON-serialise with sorted keys + trailing newline for stable diffs."""
    d = mapping.model_dump()
    d["forward"] = dict(sorted(d["forward"].items()))
    d["reverse"] = dict(sorted(d["reverse"].items(), key=lambda kv: int(kv[0])))
    for entry in d["reverse"].values():
        entry["exchanges"] = dict(sorted(entry["exchanges"].items(), key=lambda kv: int(kv[0])))
    return json.dumps(d, indent=2) + "\n"

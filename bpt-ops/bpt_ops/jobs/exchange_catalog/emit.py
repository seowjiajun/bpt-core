"""Emit the runtime JSON catalog read by non-C++ consumers (dashboard, etc.)."""

from __future__ import annotations

import json
import os
from pathlib import Path

from bpt_ops.jobs.exchange_catalog.model import ExchangeCatalog


def write_catalog_json(catalog: ExchangeCatalog, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "exchanges": [
            {"id": e.id, "name": e.name, "display_name": e.display_name}
            for e in sorted(catalog.exchanges, key=lambda x: x.id)
        ]
    }
    # Atomic write so a partial render can't corrupt the file
    tmp = target.with_suffix(target.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, indent=2) + "\n")
    os.replace(tmp, target)

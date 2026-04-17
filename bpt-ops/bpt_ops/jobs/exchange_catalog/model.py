"""Load and validate messages/exchanges.yaml."""
from __future__ import annotations

from pathlib import Path

import yaml
from pydantic import BaseModel, ConfigDict, Field


class Exchange(BaseModel):
    model_config = ConfigDict(extra="forbid")

    id: int = Field(ge=1, le=255)  # fits in SBE uint8
    name: str                       # UPPERCASE wire-format name
    display_name: str               # human-readable


class ExchangeCatalog(BaseModel):
    model_config = ConfigDict(extra="forbid")

    exchanges: list[Exchange]


def load(path: Path) -> ExchangeCatalog:
    raw = yaml.safe_load(path.read_text())
    catalog = ExchangeCatalog(**raw)

    # Invariants: unique IDs, unique names, UPPERCASE wire names.
    ids = [e.id for e in catalog.exchanges]
    names = [e.name for e in catalog.exchanges]
    if len(ids) != len(set(ids)):
        raise ValueError(f"duplicate exchange IDs in {path}: {ids}")
    if len(names) != len(set(names)):
        raise ValueError(f"duplicate exchange names in {path}: {names}")
    for e in catalog.exchanges:
        if e.name != e.name.upper() or not e.name.isidentifier():
            raise ValueError(
                f"exchange name {e.name!r} must be an UPPERCASE identifier"
            )

    return catalog

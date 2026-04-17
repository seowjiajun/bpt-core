"""Shared data-model contracts between bpt-ops producers and C++ consumers.

These schemas define the wire format that `bpt-refdata` expects to parse.
Any change here is a change to a cross-language contract — update both sides
in the same PR.
"""
from __future__ import annotations

from enum import IntEnum
from typing import Annotated

from pydantic import BaseModel, ConfigDict, Field


# Must match messages/generated/cpp/messages/ExchangeId.h (values are stable wire IDs).
class ExchangeId(IntEnum):
    BINANCE = 1
    OKX = 2
    HYPERLIQUID = 3
    DERIBIT = 4


class InstrumentType(str):
    SPOT = "SPOT"
    PERP = "PERP"
    FUTURE = "FUTURE"
    OPTION = "OPTION"


class ReverseEntry(BaseModel):
    """Canonical-id → per-venue instrument symbols + instrument metadata."""

    model_config = ConfigDict(extra="forbid")

    base: str
    quote: str
    type: str  # one of InstrumentType values
    exchanges: dict[str, str]  # str(exchange_id) -> raw venue symbol


class InstrumentMapping(BaseModel):
    """The JSON shape that bpt-refdata's InstrumentMappingLoader parses.

    forward:  "<exchange_id>_<raw_symbol>" -> canonical_id (uint32)
    reverse:  "<canonical_id>" -> ReverseEntry
    exported_at: producer timestamp (ms since epoch)
    instrument_count: len(reverse) — recomputed on write for self-consistency
    """

    model_config = ConfigDict(extra="forbid")

    forward: dict[str, Annotated[int, Field(ge=0, le=2**32 - 1)]]
    reverse: dict[str, ReverseEntry]
    exported_at: int
    instrument_count: int

"""Shared data-model contracts between bpt-ops producers and C++ consumers.

These schemas define the wire format that `bpt-refdata` expects to parse.
Any change here is a change to a cross-language contract — update both sides
in the same PR.
"""
from __future__ import annotations

from enum import IntEnum
from typing import Annotated

from pydantic import BaseModel, ConfigDict, Field, model_validator


# Must match messages/generated/cpp/messages/ExchangeId.h (values are stable wire IDs).
class ExchangeId(IntEnum):
    BINANCE = 1
    OKX = 2
    HYPERLIQUID = 3
    DERIBIT = 4


VALID_INSTRUMENT_TYPES: frozenset[str] = frozenset({"SPOT", "PERP", "FUTURES"})


class ReverseEntry(BaseModel):
    """Canonical-id → per-venue instrument symbols + instrument metadata."""

    model_config = ConfigDict(extra="forbid")

    base: str
    quote: str
    type: str
    exchanges: dict[str, str]  # str(exchange_id) -> raw venue symbol

    @model_validator(mode="after")
    def _type_in_valid_set(self) -> "ReverseEntry":
        if self.type not in VALID_INSTRUMENT_TYPES:
            raise ValueError(
                f"invalid instrument type {self.type!r}; expected one of {sorted(VALID_INSTRUMENT_TYPES)}"
            )
        return self


class InstrumentMapping(BaseModel):
    """The JSON shape that bpt-refdata's InstrumentMappingLoader parses.

    forward:  "<exchange_id>_<raw_symbol>" -> canonical_id (uint32)
              (for Binance SPOT: "<exchange_id>_<symbol>_SPOT" to avoid PERP collision)
    reverse:  "<canonical_id>" -> ReverseEntry
    exported_at: producer timestamp (ms since epoch)
    instrument_count: len(reverse) — must match, recomputed on write
    """

    model_config = ConfigDict(extra="forbid")

    forward: dict[str, Annotated[int, Field(ge=0, le=2**32 - 1)]]
    reverse: dict[str, ReverseEntry]
    exported_at: int
    instrument_count: int

    @model_validator(mode="after")
    def _count_matches_reverse_size(self) -> "InstrumentMapping":
        if self.instrument_count != len(self.reverse):
            raise ValueError(
                f"instrument_count ({self.instrument_count}) != len(reverse) ({len(self.reverse)})"
            )
        return self

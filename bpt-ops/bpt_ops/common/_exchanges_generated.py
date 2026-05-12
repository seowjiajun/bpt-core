# AUTO-GENERATED — DO NOT EDIT BY HAND.
# Regenerate with: bpt-ops exchange-catalog generate-python
# Source of truth: messages/exchanges.yaml
"""Exchange IDs — wire-format enum generated from messages/exchanges.yaml."""

from __future__ import annotations

from enum import IntEnum


class ExchangeId(IntEnum):
    BINANCE = 1
    OKX = 2
    HYPERLIQUID = 3
    DERIBIT = 4


EXCHANGE_DISPLAY_NAMES: dict[ExchangeId, str] = {
    ExchangeId.BINANCE: "Binance",
    ExchangeId.OKX: "OKX",
    ExchangeId.HYPERLIQUID: "Hyperliquid",
    ExchangeId.DERIBIT: "Deribit",
}

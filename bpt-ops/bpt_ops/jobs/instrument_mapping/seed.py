"""Pin canonical IDs for instruments that must never renumber.

The top-of-book names (BTC, ETH, SOL) get hard-coded IDs. This matters
because strategies, logs, and audit trails all reference canonical IDs —
if id=2001 ever became anything other than BTC-USDT SPOT, we'd have a
cross-temporal meaning mismatch that's effectively unfixable.

apply_seeds() is idempotent: seeds with an existing reverse entry are
skipped; only missing exchange entries + forward keys are filled in.
"""

from __future__ import annotations

from bpt_ops.common.schema import ExchangeId, InstrumentMapping
from bpt_ops.jobs.instrument_mapping.canonical_ids import ensure_reverse_entry
from bpt_ops.jobs.instrument_mapping.forward_key import build_forward_key


# (canonical_id, base, quote, type, {ExchangeId: venue_symbol})
_SEEDS: list[tuple[int, str, str, str, dict[ExchangeId, str]]] = [
    # PERP — 1001-1009 reserved for top tier
    (
        1001,
        "BTC",
        "USDT",
        "PERP",
        {
            ExchangeId.BINANCE: "BTCUSDT",
            ExchangeId.OKX: "BTC-USDT-SWAP",
            ExchangeId.HYPERLIQUID: "BTC",
        },
    ),
    (
        1002,
        "ETH",
        "USDT",
        "PERP",
        {
            ExchangeId.BINANCE: "ETHUSDT",
            ExchangeId.OKX: "ETH-USDT-SWAP",
            ExchangeId.HYPERLIQUID: "ETH",
        },
    ),
    (
        1003,
        "SOL",
        "USDT",
        "PERP",
        {
            ExchangeId.BINANCE: "SOLUSDT",
            ExchangeId.OKX: "SOL-USDT-SWAP",
            ExchangeId.HYPERLIQUID: "SOL",
        },
    ),
    # SPOT — 2001-2009 reserved for top tier
    (
        2001,
        "BTC",
        "USDT",
        "SPOT",
        {
            ExchangeId.BINANCE: "BTCUSDT",
            ExchangeId.OKX: "BTC-USDT",
        },
    ),
    (
        2002,
        "ETH",
        "USDT",
        "SPOT",
        {
            ExchangeId.BINANCE: "ETHUSDT",
            ExchangeId.OKX: "ETH-USDT",
        },
    ),
    (
        2003,
        "SOL",
        "USDT",
        "SPOT",
        {
            ExchangeId.BINANCE: "SOLUSDT",
            ExchangeId.OKX: "SOL-USDT",
        },
    ),
]


def apply_seeds(mapping: InstrumentMapping) -> InstrumentMapping:
    """Idempotently ensure every seed has a reverse entry + forward keys."""
    for canonical_id, base, quote, inst_type, exchanges in _SEEDS:
        entry = ensure_reverse_entry(mapping, canonical_id, base, quote, inst_type)
        for ex, symbol in exchanges.items():
            ex_str = str(int(ex))
            if ex_str not in entry.exchanges:
                entry.exchanges[ex_str] = symbol
            fwd_key = build_forward_key(ex, symbol, inst_type)
            if fwd_key not in mapping.forward:
                mapping.forward[fwd_key] = canonical_id
    return mapping

#pragma once

#include "strategy/refdata/instrument.h"
#include "strategy/refdata/instrument_cache.h"

#include <optional>
#include <string>
#include <vector>

namespace bpt::strategy::strategy {

// Resolves human-readable canonical instrument symbols to instrument IDs.
//
// Format:  BASE/QUOTE:TYPE
// Examples: BTC/USDT:SPOT   ETH/USDT:PERP   SOL/USDT:SPOT
//
// TYPE values (case-insensitive):
//   SPOT              → InstrumentType::SPOT
//   PERP | PERPETUAL  → InstrumentType::PERPETUAL
//   FUT  | FUTURE     → InstrumentType::FUTURE
//   OPT  | OPTION     → InstrumentType::OPTION
//
// Resolution matches on base_currency, quote_currency, and type from the
// Sindri snapshot.  Exchange filtering is applied separately via md_exchanges.
class CanonicalResolver {
public:
    struct ParsedSymbol {
        std::string base;
        std::string quote;
        refdata::InstrumentType type;
    };

    // Parse a canonical symbol string.  Returns nullopt and logs a warning on
    // malformed input so callers can skip invalid entries gracefully.
    [[nodiscard]] static std::optional<ParsedSymbol> parse(const std::string& canonical);

    // Return all instrument IDs from cache that match every element of
    // canonical_symbols restricted to the given exchanges.
    //
    // If canonical_symbols is empty — all instruments in exchanges are returned.
    // If exchanges is empty       — all instruments matching canonical_symbols.
    // If both empty               — entire cache (subscribe-all fallback).
    [[nodiscard]] static std::vector<uint64_t> resolve(const refdata::InstrumentCache& cache,
                                                       const std::vector<std::string>& canonical_symbols,
                                                       const std::vector<std::string>& exchanges);
};

}  // namespace bpt::strategy::strategy

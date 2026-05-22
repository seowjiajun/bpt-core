#pragma once

#include "strategy/refdata/i_refdata_client.h"
#include "strategy/refdata/instrument.h"
#include "strategy/refdata/instrument_cache.h"

#include <messages/ExchangeId.h>

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

    // Build the filter list passed to `IRefdataClient::subscribe()`.
    // Empty `exchanges` → single any-exchange entry per symbol.
    // Malformed symbols are skipped.
    [[nodiscard]] static std::vector<refdata::IRefdataClient::CanonicalFilter> build_filters(
        const std::vector<std::string>& canonical_symbols,
        const std::vector<std::string>& exchanges);

    // resolve() + cache lookup + exchange-id mapping in one pass.
    // Instrument held by value since cache.get() returns optional<>.
    struct ResolvedInstrument {
        uint64_t instrument_id;
        refdata::Instrument instrument;
        bpt::messages::ExchangeId::Value exchange_id;
    };
    [[nodiscard]] static std::vector<ResolvedInstrument> resolve_instruments(
        const refdata::InstrumentCache& cache,
        const std::vector<std::string>& canonical_symbols,
        const std::vector<std::string>& exchanges);

    // Single-instrument predicate: does `inst` belong to the universe?
    // Same filter semantics as resolve(). Empty lists = no constraint.
    [[nodiscard]] static bool matches(const std::vector<std::string>& canonical_symbols,
                                      const std::vector<std::string>& exchanges,
                                      const refdata::Instrument& inst);
};

}  // namespace bpt::strategy::strategy

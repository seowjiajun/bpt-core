#include "strategy/strategy/canonical_resolver.h"

#include <algorithm>
#include <yggdrasil/logging.h>

namespace bpt::strategy::strategy {

namespace {

refdata::InstrumentType parse_type(const std::string& s) {
    if (s == "SPOT")
        return refdata::InstrumentType::SPOT;
    if (s == "PERP" || s == "PERPETUAL")
        return refdata::InstrumentType::PERPETUAL;
    if (s == "FUT" || s == "FUTURE")
        return refdata::InstrumentType::FUTURE;
    if (s == "OPT" || s == "OPTION")
        return refdata::InstrumentType::OPTION;
    return refdata::InstrumentType::SPOT;
}

}  // namespace

std::optional<CanonicalResolver::ParsedSymbol> CanonicalResolver::parse(const std::string& canonical) {
    const auto slash = canonical.find('/');
    const auto colon = canonical.find(':');

    if (slash == std::string::npos || colon == std::string::npos || colon <= slash) {
        ygg::log::warn("[CanonicalResolver] Malformed canonical symbol '{}' — expected BASE/QUOTE:TYPE", canonical);
        return std::nullopt;
    }

    ParsedSymbol sym;
    sym.base = canonical.substr(0, slash);
    sym.quote = canonical.substr(slash + 1, colon - slash - 1);

    std::string type_str = canonical.substr(colon + 1);
    std::transform(type_str.begin(), type_str.end(), type_str.begin(), ::toupper);
    sym.type = parse_type(type_str);

    return sym;
}

std::vector<uint64_t> CanonicalResolver::resolve(const refdata::InstrumentCache& cache,
                                                 const std::vector<std::string>& canonical_symbols,
                                                 const std::vector<std::string>& exchanges) {
    // Pre-parse canonical symbols once.
    std::vector<ParsedSymbol> parsed;
    parsed.reserve(canonical_symbols.size());
    for (const auto& s : canonical_symbols) {
        if (auto sym = parse(s))
            parsed.push_back(*sym);
    }

    std::vector<uint64_t> result;

    for (const auto& inst : cache.get_all()) {
        // Exchange filter
        if (!exchanges.empty()) {
            const bool wanted = std::find(exchanges.begin(), exchanges.end(), inst.exchange) != exchanges.end();
            if (!wanted)
                continue;
        }

        // Canonical symbol filter
        if (!parsed.empty()) {
            bool matched = false;
            for (const auto& sym : parsed) {
                if (inst.base_currency == sym.base && inst.quote_currency == sym.quote && inst.type == sym.type) {
                    matched = true;
                    break;
                }
            }
            if (!matched)
                continue;
        }

        result.push_back(inst.instrument_id);
    }

    return result;
}

}  // namespace bpt::strategy::strategy

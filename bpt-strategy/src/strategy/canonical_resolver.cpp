#include "strategy/strategy/canonical_resolver.h"

#include "strategy/refdata/exchange_id.h"

#include <algorithm>
#include <bpt_common/logging.h>

namespace bpt::strategy::strategy {

namespace {

bpt::messages::InstrumentType::Value to_sbe_type(refdata::InstrumentType t) {
    using T = refdata::InstrumentType;
    using S = bpt::messages::InstrumentType;
    switch (t) {
        case T::SPOT:
            return S::SPOT;
        case T::PERPETUAL:
            return S::PERPETUAL;
        case T::FUTURE:
            return S::FUTURE;
        case T::OPTION:
            return S::OPTION;
        default:
            return S::NULL_VALUE;
    }
}

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
        bpt::common::log::warn("[CanonicalResolver] Malformed canonical symbol '{}' — expected BASE/QUOTE:TYPE",
                               canonical);
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

std::vector<refdata::IRefdataClient::CanonicalFilter> CanonicalResolver::build_filters(
    const std::vector<std::string>& canonical_symbols,
    const std::vector<std::string>& exchanges) {
    std::vector<refdata::IRefdataClient::CanonicalFilter> filters;
    filters.reserve(canonical_symbols.size() * std::max<size_t>(exchanges.size(), 1));

    for (const auto& sym : canonical_symbols) {
        auto parsed = parse(sym);
        if (!parsed)
            continue;
        const auto sbe_type = to_sbe_type(parsed->type);
        if (exchanges.empty()) {
            filters.push_back({parsed->base, parsed->quote, sbe_type, ""});
        } else {
            for (const auto& ex : exchanges)
                filters.push_back({parsed->base, parsed->quote, sbe_type, ex});
        }
    }
    return filters;
}

bool CanonicalResolver::matches(const std::vector<std::string>& canonical_symbols,
                                const std::vector<std::string>& exchanges,
                                const refdata::Instrument& inst) {
    if (!exchanges.empty()) {
        if (std::find(exchanges.begin(), exchanges.end(), inst.exchange) == exchanges.end())
            return false;
    }
    if (canonical_symbols.empty())
        return true;
    for (const auto& s : canonical_symbols) {
        auto p = parse(s);
        if (p && inst.base_currency == p->base && inst.quote_currency == p->quote && inst.type == p->type)
            return true;
    }
    return false;
}

std::vector<CanonicalResolver::ResolvedInstrument> CanonicalResolver::resolve_instruments(
    const refdata::InstrumentCache& cache,
    const std::vector<std::string>& canonical_symbols,
    const std::vector<std::string>& exchanges) {
    const auto ids = resolve(cache, canonical_symbols, exchanges);
    std::vector<ResolvedInstrument> out;
    out.reserve(ids.size());
    for (uint64_t id : ids) {
        auto inst = cache.get(id);
        if (!inst)
            continue;
        const auto ex_id = refdata::to_exchange_id(inst->exchange);
        out.push_back(ResolvedInstrument{id, std::move(*inst), ex_id});
    }
    return out;
}

}  // namespace bpt::strategy::strategy

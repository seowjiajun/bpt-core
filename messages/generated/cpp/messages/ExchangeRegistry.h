// AUTO-GENERATED — DO NOT EDIT BY HAND.
// Regenerate with: bpt-ops exchange-catalog generate-cpp-registry
// Source of truth: messages/exchanges.yaml
//
// Adds the missing half of exchange identity that ExchangeId.h doesn't carry:
// reverse lookup (string → Value), display names, and a constexpr table
// callers can iterate. Pairs with the SBE-generated ExchangeId.h, which
// already provides Value → c_str() and uint8_t → Value.
//
// Use cases:
//   - Validate a TOML's `exchange = "..."` field at config load (typo fails fast)
//   - Map enum → display name for logs, dashboards, alerting
//   - Iterate kEntries to enumerate every known venue
#pragma once

#include "ExchangeId.h"

#include <array>
#include <optional>
#include <string_view>

namespace bpt {
namespace messages {

class ExchangeRegistry
{
public:
    struct Entry
    {
        ExchangeId::Value id;
        std::string_view  name;          // UPPERCASE wire-format name (matches ExchangeId::c_str)
        std::string_view  display_name;  // human-readable, dashboard-friendly
    };

    static constexpr std::array<Entry, 4> kEntries = {{
        {ExchangeId::BINANCE, "BINANCE", "Binance"},
        {ExchangeId::OKX, "OKX", "OKX"},
        {ExchangeId::HYPERLIQUID, "HYPERLIQUID", "Hyperliquid"},
        {ExchangeId::DERIBIT, "DERIBIT", "Deribit"},
    }};

    // Wire-name → enum lookup. Returns nullopt if the name is unknown so
    // callers can fail fast at the boundary (e.g. config load).
    static constexpr std::optional<ExchangeId::Value> from_name(std::string_view s) noexcept
    {
        for (const auto& e : kEntries)
            if (e.name == s)
                return e.id;
        return std::nullopt;
    }

    // Enum → display name. Returns empty view for unknown values; callers
    // typically pass values that came from from_name(), so unknown is a
    // logic-error path. Use ExchangeId::c_str for the wire-format name.
    static constexpr std::string_view display_name(ExchangeId::Value id) noexcept
    {
        for (const auto& e : kEntries)
            if (e.id == id)
                return e.display_name;
        return {};
    }
};

}  // namespace messages
}  // namespace bpt

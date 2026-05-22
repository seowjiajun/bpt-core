#pragma once

#include <messages/ExchangeId.h>

#include <string_view>

namespace bpt::strategy::refdata {

// Returns NULL_VALUE for unknown names.
inline bpt::messages::ExchangeId::Value to_exchange_id(std::string_view name) {
    using bpt::messages::ExchangeId;
    if (name == "BINANCE")
        return ExchangeId::BINANCE;
    if (name == "OKX")
        return ExchangeId::OKX;
    if (name == "HYPERLIQUID")
        return ExchangeId::HYPERLIQUID;
    if (name == "DERIBIT")
        return ExchangeId::DERIBIT;
    return ExchangeId::NULL_VALUE;
}

}  // namespace bpt::strategy::refdata

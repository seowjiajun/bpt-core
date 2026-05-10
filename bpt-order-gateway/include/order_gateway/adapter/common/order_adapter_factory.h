#pragma once

/// @file
/// Factory function mapping ExchangeId enum value to the venue-specific
/// OrderAdapter subclass. Same shape as md_adapter_factory in mdgw.
/// Returns nullptr for unknown venue (caller throws).

#include "order_gateway/adapter/binance/binance_order_adapter.h"
#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/adapter/deribit/deribit_order_adapter.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_order_adapter.h"
#include "order_gateway/adapter/okx/okx_order_adapter.h"
#include <messages/ExchangeRegistry.h>

#include <memory>

namespace bpt::order_gateway::adapter {

/// @brief Build the venue-specific OrderAdapter for `exch_id`.
///
/// Order-side adapters aren't templated (no equivalent of mdgw's Pub
/// parameter) — concrete subclasses suffice. Each takes the same
/// (cfg, exchange_creds) constructor signature.
inline std::shared_ptr<IOrderAdapter> make_order_adapter(
    bpt::messages::ExchangeId::Value exch_id,
    const bpt::order_gateway::config::AdapterConfig& cfg,
    const ExchangeCredentials& creds) {
    using namespace bpt::messages;
    switch (exch_id) {
        case ExchangeId::BINANCE:
            return std::make_shared<BinanceOrderAdapter>(cfg, creds);
        case ExchangeId::OKX:
            return std::make_shared<OKXOrderAdapter>(cfg, creds);
        case ExchangeId::DERIBIT:
            return std::make_shared<DeribitOrderAdapter>(cfg, creds);
        case ExchangeId::HYPERLIQUID:
            return std::make_shared<HyperliquidOrderAdapter>(cfg, creds);
        default:
            return nullptr;
    }
}

/// @brief Map ExchangeId enum to the bitmask flag used in heartbeat
/// status reports. Bit positions are part of the heartbeat schema
/// contract — don't reshuffle without coordinating with consumers.
///
/// Returns 0 for unknown / unmapped venues (caller can decide whether
/// to error or just skip).
inline uint8_t exchange_status_bit(bpt::messages::ExchangeId::Value exch_id) {
    using namespace bpt::messages;
    switch (exch_id) {
        case ExchangeId::BINANCE:     return 0x01;
        case ExchangeId::OKX:         return 0x02;
        case ExchangeId::HYPERLIQUID: return 0x04;
        case ExchangeId::DERIBIT:     return 0x08;
        default:                      return 0x00;
    }
}

}  // namespace bpt::order_gateway::adapter

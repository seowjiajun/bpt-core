#pragma once

/// @file
/// Factory function that maps an ExchangeId enum value to the
/// venue-specific MdAdapter subclass. Replaces the per-venue switch
/// that used to live in MdGatewayService's constructor; mirror of the
/// bpt-tape `make_recording_adapter` extraction in the same spirit.
///
/// Templated on Pub (the publisher type) so callers using
/// MdPublisher (live mdgw) and any future variant (e.g. backtester
/// harness) share the same factory.

#include "md_gateway/adapter/binance/binance_md_adapter.h"
#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/adapter/deribit/deribit_md_adapter.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_md_adapter.h"
#include "md_gateway/adapter/okx/okx_md_adapter.h"

#include <messages/ExchangeRegistry.h>

#include <memory>

namespace bpt::md_gateway::adapter {

/// @brief Build the venue-specific MdAdapter for `exch_id`.
///
/// Returns nullptr if the exchange is in messages/exchanges.yaml but
/// mdgw has no adapter for it (caller throws). `exch_id` is the
/// ExchangeId::Value enum (what ExchangeRegistry::from_name() returns
/// through std::optional).
template <class Pub>
inline std::shared_ptr<IAdapter> make_md_adapter(bpt::messages::ExchangeId::Value exch_id,
                                                 const bpt::md_gateway::config::AdapterConfig& cfg,
                                                 std::shared_ptr<Pub> md_pub) {
    using namespace bpt::messages;
    switch (exch_id) {
        case ExchangeId::BINANCE:
            return std::make_shared<BinanceMdAdapter<Pub>>(cfg, md_pub);
        case ExchangeId::OKX:
            return std::make_shared<OkxMdAdapter<Pub>>(cfg, md_pub);
        case ExchangeId::HYPERLIQUID:
            return std::make_shared<HyperliquidMdAdapter<Pub>>(cfg, md_pub);
        case ExchangeId::DERIBIT:
            return std::make_shared<DeribitMdAdapter<Pub>>(cfg, md_pub);
        default:
            return nullptr;
    }
}

}  // namespace bpt::md_gateway::adapter

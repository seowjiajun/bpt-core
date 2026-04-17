#pragma once

#include "md_gateway/adapter/common/i_exchange_parser.h"
#include "md_gateway/adapter/common/subscription_map.h"

#include <yggdrasil/util/latency_histogram.h>

namespace bpt::md_gateway::adapter {

// Parses Hyperliquid WebSocket frames.
//
// Handled channels:
//   l2Book          → publish_bbo (top of book only)
//   trades          → publish_trade (one publish per trade in the array)
//   activeAssetCtx  → on_funding_rate callback (no nextFundingTime from HL)
class HyperliquidParser : public IExchangeParser {
public:
    explicit HyperliquidParser(const SubscriptionMap& subs) : subs_(subs) {}

    void parse(std::string_view payload,
               uint64_t recv_ns,
               messaging::IMdPublisher& pub,
               messaging::FundingRateCallback& on_funding_rate) override;

    ygg::util::LatencyHistogram decode_lat_;

private:
    const SubscriptionMap& subs_;
    uint64_t tick_count_{0};
};

}  // namespace bpt::md_gateway::adapter

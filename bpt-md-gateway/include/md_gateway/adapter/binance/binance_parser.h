#pragma once

#include "md_gateway/adapter/common/i_exchange_parser.h"
#include "md_gateway/adapter/common/subscription_map.h"

#include <yggdrasil/util/latency_histogram.h>

namespace bpt::md_gateway::adapter {

// Parses Binance combined-stream WebSocket frames.
//
// Handled message types:
//   <sym>@bookTicker  → publish_bbo
//   <sym>@aggTrade    → publish_trade
//
// Funding rates arrive on a separate BinanceAdapter thread
// (fstream.binance.com) and are not handled here.
class BinanceParser : public IExchangeParser {
public:
    explicit BinanceParser(const SubscriptionMap& subs) : subs_(subs) {}

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

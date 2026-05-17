#pragma once

/// @file
/// Aeron-backed bpt-radar MarketColor subscriber.

#include "bridge/messaging/subscribers/api/market_color_subscriber.h"

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging::aeron {

class MarketColorSubscriber final : public api::MarketColorSubscriber {
public:
    MarketColorSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    int poll(int fragment_limit = 4) override;

private:
    std::shared_ptr<::aeron::Subscription> sub_;
};

}  // namespace bpt::bridge::messaging::aeron

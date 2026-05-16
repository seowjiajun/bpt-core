#pragma once

/// \file
/// \brief Publishes MarketColor POD frames to stream `market_color` (typically 6002).
///
/// Periodic, latest-wins. Drop-on-no-subscriber is fine — the next interval
/// produces a fresh snapshot.

#include "radar/messaging/codecs/pod_market_color_codec.h"
#include "radar/messaging/market_color.h"
#include "radar/messaging/publishers/api/market_color_publisher.h"

#include <bpt_common/aeron/publisher.h>
#include <memory>
#include <string>

namespace bpt::radar::messaging::aeron {

class MarketColorPublisher final : public api::MarketColorPublisher {
public:
    MarketColorPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// \brief Returns true if Aeron accepted the offer.
    bool publish(const MarketColor& color) override;

private:
    std::unique_ptr<bpt::common::aeron::Publisher> pub_;
    PodMarketColorCodec                            codec_;
};

}  // namespace bpt::radar::messaging::aeron

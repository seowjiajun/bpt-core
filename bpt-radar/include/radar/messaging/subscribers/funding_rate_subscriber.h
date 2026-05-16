#pragma once

/// \file
/// \brief Subscribes to bpt-md-gateway's FundingRate stream (typically 1005).
///
/// SBE-decoded; consumer gets (instrument_id, rate_bps, next_funding_ts_ns).
/// Used by radar to fill MarketColor's perp_funding_rate_8h and
/// perp_next_funding_ts_ns fields after joining instrument_id to underlying
/// via the refdata snapshot.

#include <Aeron.h>

#include <messages/FundingRate.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::radar::messaging {

class FundingRateSubscriber {
public:
    using OnFundingFn = std::function<void(bpt::messages::FundingRate&)>;

    FundingRateSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    int poll(int fragment_limit = 8);

    OnFundingFn on_funding;

private:
    void handle_fragment(aeron::AtomicBuffer& buffer,
                         aeron::util::index_t offset,
                         aeron::util::index_t length,
                         aeron::Header& header);

    std::shared_ptr<aeron::Subscription> sub_;
};

}  // namespace bpt::radar::messaging

#pragma once

/// \file
/// Port: refdata subscriber. The per-frame dispatch path was lifted to
/// a CRTP-templated concrete subscriber — see
/// `aeron::RefdataSubscriber<Handler>` (Handler is `PricerService` in
/// prod). Discovers option instruments from bpt-refdata snapshot +
/// delta streams; surfaces perp metadata too (used to map FundingRate
/// streams to underlyings).

#include "pricer/surface/surface_builder.h"

#include <messages/ExchangeId.h>

#include <cstdint>
#include <string>

namespace bpt::pricer::refdata {

struct PerpInstrument {
    uint64_t instrument_id;
    std::string underlying;
    std::string exchange;
    bpt::messages::ExchangeId::Value exchange_id;
    std::string venue_symbol;  ///< e.g. "BTC-PERPETUAL"; needed by MdSubscribeBatch
};

namespace api {

class RefdataSubscriber {
public:
    virtual ~RefdataSubscriber() = default;

    /// Publish a RefDataSubscriptionRequest on the control stream to ask
    /// bpt-refdata to push the current snapshot. Requires the control
    /// publication to have been configured via the constructor; no-op
    /// otherwise.
    virtual void send_subscription_request(uint64_t correlation_id) = 0;

    /// Poll both snapshot and delta subscriptions.
    virtual int poll(int fragment_limit = 10) = 0;
};

}  // namespace api

}  // namespace bpt::pricer::refdata

#pragma once

/// \file
/// Port: FundingRate subscriber. CRTP-templated concrete in
/// `aeron::FundingRateSubscriber<H>`.

namespace bpt::radar::messaging::api {

class FundingRateSubscriber {
public:
    virtual ~FundingRateSubscriber() = default;

    virtual int poll(int fragment_limit = 8) = 0;
};

}  // namespace bpt::radar::messaging::api

#pragma once

/// \file
/// Port: BBO subscriber. CRTP-templated concrete in
/// `aeron::MdMarketDataSubscriber<H>`.

namespace bpt::radar::messaging::api {

class MdMarketDataSubscriber {
public:
    virtual ~MdMarketDataSubscriber() = default;

    virtual int poll(int fragment_limit = 16) = 0;
};

}  // namespace bpt::radar::messaging::api

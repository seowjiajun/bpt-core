#pragma once

/// \file
/// Port: trade subscriber. CRTP-templated concrete in
/// `aeron::MdTradeSubscriber<H>`.

namespace bpt::radar::messaging::api {

class MdTradeSubscriber {
public:
    virtual ~MdTradeSubscriber() = default;

    virtual int poll(int fragment_limit = 16) = 0;
};

}  // namespace bpt::radar::messaging::api

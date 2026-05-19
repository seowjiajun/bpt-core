#pragma once

/// @file
/// Port: bpt-radar MarketColor subscriber. CRTP-templated concrete in
/// `aeron::MarketColorSubscriber<H>` calls H::on_market_color.

namespace bpt::bridge::messaging::api {

class MarketColorSubscriber {
public:
    virtual ~MarketColorSubscriber() = default;

    virtual int poll(int fragment_limit = 4) = 0;
};

}  // namespace bpt::bridge::messaging::api

#pragma once

/// @file
/// Port: strategy portfolio-snapshot JSON subscriber. CRTP-templated
/// concrete in `aeron::PortfolioSnapshotSubscriber<H>` calls
/// H::on_portfolio_json. Multi-fragment reassembly inside the concrete.

namespace bpt::bridge::messaging::api {

class PortfolioSnapshotSubscriber {
public:
    virtual ~PortfolioSnapshotSubscriber() = default;

    virtual int poll(int fragment_limit = 1) = 0;
};

}  // namespace bpt::bridge::messaging::api

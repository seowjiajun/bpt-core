#pragma once

/// @file
/// Port: strategy portfolio-snapshot JSON subscriber. Multi-fragment
/// reassembly happens inside the Aeron concrete; this port just
/// surfaces the reassembled string view.

#include <functional>
#include <string_view>

namespace bpt::bridge::messaging::api {

class PortfolioSnapshotSubscriber {
public:
    using JsonHandler = std::function<void(std::string_view json)>;

    virtual ~PortfolioSnapshotSubscriber() = default;

    void set_handler(JsonHandler h) { handler_ = std::move(h); }

    virtual int poll(int fragment_limit = 1) = 0;

protected:
    JsonHandler handler_;
};

}  // namespace bpt::bridge::messaging::api

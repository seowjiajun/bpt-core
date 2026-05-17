#pragma once

/// @file
/// Port: MdGateway MD-data subscriber. Delivers a (instrument_id,
/// mid_price, ts_ns) tick callback.

#include <cstdint>
#include <functional>

namespace bpt::bridge::messaging::api {

class MdSubscriber {
public:
    /// (instrument_id, mid_price, ts_ns)
    using TickHandler = std::function<void(uint64_t, double, uint64_t)>;

    virtual ~MdSubscriber() = default;

    void set_handler(TickHandler h) { handler_ = std::move(h); }

    virtual int poll(int fragment_limit = 32) = 0;

protected:
    TickHandler handler_;
};

}  // namespace bpt::bridge::messaging::api

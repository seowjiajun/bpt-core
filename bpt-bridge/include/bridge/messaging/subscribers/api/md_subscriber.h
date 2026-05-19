#pragma once

/// @file
/// Port: MdGateway MD-data subscriber. CRTP-templated concrete in
/// `aeron::MdSubscriber<H>` delivers a (instrument_id, mid_price, ts_ns)
/// tick directly to the Handler's on_md_tick method.

namespace bpt::bridge::messaging::api {

class MdSubscriber {
public:
    virtual ~MdSubscriber() = default;

    virtual int poll(int fragment_limit = 32) = 0;
};

}  // namespace bpt::bridge::messaging::api

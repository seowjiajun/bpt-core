#pragma once

/// \file
/// Port: InstrumentStats subscriber. CRTP-templated concrete in
/// `aeron::InstrumentStatsSubscriber<H>`.

namespace bpt::radar::messaging::api {

class InstrumentStatsSubscriber {
public:
    virtual ~InstrumentStatsSubscriber() = default;

    virtual int poll(int fragment_limit = 8) = 0;
};

}  // namespace bpt::radar::messaging::api

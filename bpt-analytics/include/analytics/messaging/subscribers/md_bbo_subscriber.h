#pragma once

/// @file
/// Subscriber for the bpt-md-gateway BBO stream. Decodes SBE
/// `MdMarketData` and exposes a domain-shaped callback (instrument_id,
/// bid, ask, timestamp_ns). Lifts the inline subscriber + handle_md
/// combo from `analytics_service.cpp` so the app stops pulling `<Aeron.h>` into
/// its header.

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::analytics::messaging {

class MdBboSubscriber {
public:
    using OnBboFn = std::function<void(uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns)>;

    MdBboSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    int poll(int fragment_limit = 10);

    OnBboFn on_bbo;

private:
    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
};

}  // namespace bpt::analytics::messaging

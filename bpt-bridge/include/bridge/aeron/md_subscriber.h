#pragma once

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::bridge {

// Subscribes to MdGateway's MD data stream and delivers a mid-price tick callback.
// Thin wrapper around the Aeron subscription — all decoding happens inline.
class MdSubscriber {
public:
    // (instrument_id, mid_price, ts_ns)
    using TickHandler = std::function<void(uint64_t, double, uint64_t)>;

    MdSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    void set_handler(TickHandler h) { handler_ = std::move(h); }

    // Poll the Aeron subscription; call from the main loop.
    int poll(int fragment_limit = 32);

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    TickHandler handler_;
};

}  // namespace bpt::bridge

#pragma once

#include <Aeron.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace bpt::pricer::md {

// Passive MD subscriber — reads BBO ticks from Huginn's stream 2002.
// Pricer is a read-only consumer; it does not send subscription requests.
// (Strategy's subscriptions drive what Huginn publishes.)
class MdSubscriber {
public:
    using BboCallback = std::function<void(uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns)>;
    using TradeCallback = std::function<void(uint64_t instrument_id, double price, double qty, uint64_t timestamp_ns)>;

    MdSubscriber(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    void set_bbo_callback(BboCallback cb) { on_bbo_ = std::move(cb); }
    void set_trade_callback(TradeCallback cb) { on_trade_ = std::move(cb); }

    // Poll for fragments. Returns number of fragments processed.
    int poll(int fragment_limit = 10);

private:
    void on_fragment(const aeron::concurrent::AtomicBuffer& buffer,
                     aeron::util::index_t offset,
                     aeron::util::index_t length,
                     const aeron::Header& header);

    std::shared_ptr<aeron::Subscription> sub_;
    BboCallback on_bbo_;
    TradeCallback on_trade_;
};

}  // namespace bpt::pricer::md

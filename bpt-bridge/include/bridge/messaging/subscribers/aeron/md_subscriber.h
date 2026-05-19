#pragma once

/// @file
/// Aeron-backed MdGateway MD-data subscriber. CRTP-templated on the
/// Handler — in prod the Handler is `BridgeService` and per-tick
/// dispatch is direct.

#include "bridge/aeron/sbe_decode.h"
#include "bridge/messaging/subscribers/api/md_subscriber.h"

#include <Aeron.h>

#include <messages/MdMarketData.h>

#include <bpt_common/aeron/subscriber.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::bridge::messaging::aeron {

template <class Handler>
class MdSubscriber final : public api::MdSubscriber {
public:
    MdSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id) {
        sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            std::move(aeron),
            channel,
            stream_id,
            [this](::aeron::AtomicBuffer& b, ::aeron::util::index_t o, ::aeron::util::index_t l, ::aeron::Header& h) {
                on_fragment(b, o, l, h);
            });
        bpt::common::log::info("[bridge/MD] subscribed on {} stream {}", channel, stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 32) override { return sub_ ? sub_->poll(fragment_limit) : 0; }

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& /*header*/) {
        decode_sbe_fragment<bpt::messages::MdMarketData>(
            buffer, offset, length, [this](bpt::messages::MdMarketData& md) {
                const double bid = md.bidPrice();
                const double ask = md.askPrice();
                if (bid <= 0 || ask <= 0)
                    return;
                const double mid = (bid + ask) * 0.5;
                if (handler_ != nullptr) [[likely]]
                    handler_->on_md_tick(md.instrumentId(), mid, md.timestampNs());
            });
    }

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::bridge::messaging::aeron

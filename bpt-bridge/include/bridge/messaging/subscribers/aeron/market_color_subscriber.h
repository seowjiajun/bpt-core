#pragma once

/// @file
/// Aeron-backed bpt-radar MarketColor subscriber. CRTP-templated.

#include "bridge/messaging/subscribers/api/market_color_subscriber.h"

#include <Aeron.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <radar/messaging/market_color.h>
#include <string>
#include <utility>

namespace bpt::bridge::messaging::aeron {

template <class Handler>
class MarketColorSubscriber final : public api::MarketColorSubscriber {
public:
    MarketColorSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id) {
        sub_ = bpt::common::aeron::wait_for_subscription(std::move(aeron), channel, stream_id);
        bpt::common::log::info("[bridge/MarketColor] subscribed on {} stream {}", channel, stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 4) override {
        if (!sub_)
            return 0;
        return sub_->poll(
            [this](::aeron::AtomicBuffer& buffer,
                   ::aeron::util::index_t offset,
                   ::aeron::util::index_t length,
                   ::aeron::Header& /*hdr*/) {
                if (static_cast<std::size_t>(length) != sizeof(bpt::radar::messaging::MarketColor))
                    return;
                if (handler_ == nullptr) [[unlikely]]
                    return;
                bpt::radar::messaging::MarketColor mc;
                std::memcpy(&mc, buffer.buffer() + offset, sizeof(mc));
                handler_->on_market_color(mc);
            },
            fragment_limit);
    }

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::bridge::messaging::aeron

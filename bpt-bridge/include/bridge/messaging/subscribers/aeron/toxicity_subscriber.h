#pragma once

/// @file
/// Aeron-backed bpt-analytics ToxicityUpdate subscriber. CRTP-templated.

#include "bridge/messaging/subscribers/api/toxicity_subscriber.h"

#include <Aeron.h>

#include <analytics/messaging/toxicity_update.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/logging.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace bpt::bridge::messaging::aeron {

template <class Handler>
class ToxicitySubscriber final : public api::ToxicitySubscriber {
public:
    ToxicitySubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream) {
        sub_ = bpt::common::aeron::wait_for_subscription(std::move(aeron), stream.channel, stream.stream_id);
        bpt::common::log::info("[bridge/Toxicity] subscribed on {} stream {}", stream.channel, stream.stream_id);
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
                if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                    return;
                if (handler_ == nullptr) [[unlikely]]
                    return;
                bpt::analytics::messaging::ToxicityUpdate u;
                std::memcpy(&u, buffer.buffer() + offset, sizeof(u));
                handler_->on_toxicity(u);
            },
            fragment_limit);
    }

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::bridge::messaging::aeron

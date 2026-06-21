#pragma once

/// @file
/// Aeron-backed concrete for api::ToxicitySubscriber. Templated on the
/// Handler that receives parsed updates — in prod the Handler is
/// `StrategyService` and the per-update path inlines through to
/// `StrategyService::on_toxicity_update`.

#include "strategy/messaging/subscribers/api/toxicity_subscriber.h"

#include <Aeron.h>

#include <analytics/messaging/toxicity_update.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/logging.h>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace bpt::strategy::messaging::aeron {

template <class Handler>
class ToxicitySubscriber final : public api::ToxicitySubscriber {
public:
    ToxicitySubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream)
        : sub_(bpt::common::aeron::wait_for_subscription(std::move(aeron), stream.channel, stream.stream_id)) {
        bpt::common::log::info("Toxicity subscription ready: {} stream {}", stream.channel, stream.stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 4) override {
        if (!sub_)
            return 0;
        return sub_->poll(
            [this](const ::aeron::concurrent::AtomicBuffer& buffer,
                   ::aeron::util::index_t offset,
                   ::aeron::util::index_t length,
                   const ::aeron::Header&) {
                if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                    return;
                if (handler_ == nullptr) [[unlikely]]
                    return;
                bpt::analytics::messaging::ToxicityUpdate update;
                std::memcpy(&update, buffer.buffer() + offset, sizeof(update));
                handler_->on_toxicity_update(update);
            },
            fragment_limit);
    }

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::strategy::messaging::aeron

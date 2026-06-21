#pragma once

/// @file
/// Aeron-backed concrete for api::ConsoleControlSubscriber. Templated on
/// the Handler type that receives commands — in prod the Handler is
/// `StrategyService` and the optimiser inlines the per-command path.

#include "strategy/messaging/subscribers/api/console_control_subscriber.h"

#include <Aeron.h>

#include <bpt_common/aeron/stream_config.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace bpt::strategy::messaging::aeron {

template <class Handler>
class ConsoleControlSubscriber final : public api::ConsoleControlSubscriber {
public:
    ConsoleControlSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream) {
        // Same 5-second poll-and-wait as the inline construction the app
        // used previously. The console publisher may not be up yet; if we
        // can't bind, log at the call site (is_ready() = false) and the
        // poll loop becomes a no-op.
        const int64_t reg_id = aeron->addSubscription(stream.channel, stream.stream_id);
        for (int i = 0; i < 500; ++i) {
            sub_ = aeron->findSubscription(reg_id);
            if (sub_)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    [[nodiscard]] bool is_ready() const override { return static_cast<bool>(sub_); }

    int poll(int fragment_limit = 1) override {
        if (!sub_)
            return 0;
        return sub_->poll(
            [this](::aeron::AtomicBuffer& buffer,
                   ::aeron::util::index_t offset,
                   ::aeron::util::index_t length,
                   ::aeron::Header& /*hdr*/) {
                if (length < 1)
                    return;
                if (handler_ == nullptr) [[unlikely]]
                    return;
                const uint8_t cmd = *reinterpret_cast<const uint8_t*>(buffer.buffer() + offset);
                handler_->on_console_command(cmd);
            },
            fragment_limit);
    }

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::strategy::messaging::aeron

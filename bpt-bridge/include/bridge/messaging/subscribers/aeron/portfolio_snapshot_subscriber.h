#pragma once

/// @file
/// Aeron-backed strategy portfolio-snapshot subscriber. CRTP-templated.
/// FragmentAssembler reassembles multi-fragment JSON before invoking
/// H::on_portfolio_json(string_view).

#include "bridge/messaging/subscribers/api/portfolio_snapshot_subscriber.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/logging.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace bpt::bridge::messaging::aeron {

template <class Handler>
class PortfolioSnapshotSubscriber final : public api::PortfolioSnapshotSubscriber {
public:
    PortfolioSnapshotSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                const bpt::common::config::StreamConfig& stream) {
        sub_ = bpt::common::aeron::wait_for_subscription(std::move(aeron), stream.channel, stream.stream_id);
        bpt::common::log::info("[bridge/Portfolio] subscribed on {} stream {}", stream.channel, stream.stream_id);

        assembler_ = std::make_unique<::aeron::FragmentAssembler>([this](::aeron::AtomicBuffer& buffer,
                                                                         ::aeron::util::index_t offset,
                                                                         ::aeron::util::index_t length,
                                                                         ::aeron::Header& /*hdr*/) {
            if (handler_ == nullptr) [[unlikely]]
                return;
            std::string_view json(reinterpret_cast<const char*>(buffer.buffer() + offset),
                                  static_cast<std::size_t>(length));
            handler_->on_portfolio_json(json);
        });
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 1) override {
        if (!sub_ || !assembler_)
            return 0;
        return sub_->poll(assembler_->handler(), fragment_limit);
    }

private:
    std::shared_ptr<::aeron::Subscription> sub_;
    std::unique_ptr<::aeron::FragmentAssembler> assembler_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::bridge::messaging::aeron

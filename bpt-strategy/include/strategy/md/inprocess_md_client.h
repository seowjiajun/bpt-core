#pragma once

/// @file
/// InProcessMdClient — push-driven IMdClient for the deterministic
/// backtest harness. No Aeron, no thread, no polling. The harness
/// constructs one of these, the strategy receives it as IMdClient&,
/// and the harness drives events synchronously by calling push_bbo /
/// push_trade / push_order_book / push_heartbeat — each of which fires
/// the registered Handler's method inline before returning.
///
/// Templated on the same Handler type as the prod `AeronMdClient<H>` so
/// strategy behaviour is bit-identical across both transports.
///
/// Consequences for determinism:
///   - Single thread of control through every event from tape to
///     strategy decision and back; no scheduling can reorder.
///   - poll() returns 0 because there's nothing to drain — events
///     have already been delivered via the push_* calls.
///   - subscribe() captures the subscription so the harness can know
///     which instruments the strategy wants, but doesn't itself
///     wire any transport. The harness consults
///     subscribed_instruments() and only dispatches events for those.

#include "strategy/md/i_md_client.h"

#include <messages/MdMarketData.h>
#include <messages/MdOrderBook.h>
#include <messages/MdServiceHeartbeat.h>
#include <messages/MdTrade.h>

#include <cstdint>
#include <vector>

namespace bpt::strategy::md {

template <class Handler>
class InProcessMdClient : public IMdClient {
public:
    InProcessMdClient() = default;

    /// Bind the per-tick dispatch target. Harness sets this once at
    /// construction time before pumping events.
    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    void subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) override {
        last_correlation_id_ = correlation_id;
        subscribed_ = instruments;
    }

    /// No fragments to drain — events arrive via push_*.
    int poll(int /*fragment_limit*/ = 10) override { return 0; }

    /// Harness-side push API. Each call fires the corresponding handler
    /// method synchronously, returning when the call has fully
    /// processed the event.
    void push_bbo(const bpt::messages::MdMarketData& tick) {
        if (handler_ != nullptr) [[likely]]
            handler_->on_bbo(tick);
    }
    void push_trade(const bpt::messages::MdTrade& tick) {
        if (handler_ != nullptr) [[likely]]
            handler_->on_trade(tick);
    }
    void push_order_book(const bpt::messages::MdOrderBook& book) {
        if (handler_ != nullptr) [[likely]]
            handler_->on_order_book(book);
    }
    void push_heartbeat() {
        if (handler_ != nullptr) [[likely]]
            handler_->on_md_service_heartbeat();
    }

    [[nodiscard]] const std::vector<InstrumentDesc>& subscribed_instruments() const noexcept { return subscribed_; }
    [[nodiscard]] uint64_t last_correlation_id() const noexcept { return last_correlation_id_; }

private:
    Handler* handler_{nullptr};
    uint64_t last_correlation_id_{0};
    std::vector<InstrumentDesc> subscribed_;
};

}  // namespace bpt::strategy::md

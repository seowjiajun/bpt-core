#pragma once

/// @file
/// InProcessMdClient — push-driven IMdClient for the deterministic
/// backtest harness. No Aeron, no thread, no polling. The harness
/// constructs one of these, the strategy receives it as IMdClient&,
/// and the harness drives events synchronously by calling push_bbo /
/// push_trade / push_order_book / push_heartbeat — each of which fires
/// the strategy's registered callback inline before returning.
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

#include <messages/MdServiceHeartbeat.h>

#include <cstdint>
#include <vector>

namespace bpt::strategy::md {

class InProcessMdClient : public IMdClient {
public:
    InProcessMdClient() = default;

    void subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) override {
        last_correlation_id_ = correlation_id;
        subscribed_ = instruments;
    }

    /// No fragments to drain — events arrive via push_*.
    int poll(int /*fragment_limit*/ = 10) override { return 0; }

    /// Harness-side push API. Each call fires the corresponding
    /// strategy callback synchronously, returning when the callback
    /// has fully processed the event (including any orders the
    /// strategy fired in response — those flow through the in-process
    /// order gateway, which calls back into the matching engine, etc.,
    /// all inline on this thread).
    void push_bbo(const bpt::messages::MdMarketData& tick) {
        if (on_bbo)
            on_bbo(tick);
    }
    void push_trade(const bpt::messages::MdTrade& tick) {
        if (on_trade)
            on_trade(tick);
    }
    void push_order_book(const bpt::messages::MdOrderBook& book) {
        if (on_order_book)
            on_order_book(book);
    }
    void push_heartbeat() {
        if (on_service_heartbeat)
            on_service_heartbeat();
    }

    /// What the strategy subscribed to. Harness uses this to filter
    /// the captured tape down to the instruments the strategy cares
    /// about, avoiding wasted dispatches.
    [[nodiscard]] const std::vector<InstrumentDesc>& subscribed_instruments() const { return subscribed_; }
    [[nodiscard]] uint64_t last_correlation_id() const { return last_correlation_id_; }

private:
    uint64_t last_correlation_id_{0};
    std::vector<InstrumentDesc> subscribed_;
};

}  // namespace bpt::strategy::md

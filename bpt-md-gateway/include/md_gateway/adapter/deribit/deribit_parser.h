#pragma once

#include "md_gateway/adapter/common/i_exchange_parser.h"
#include "md_gateway/adapter/common/subscription_map.h"

#include <atomic>
#include <string>
#include <unordered_map>
#include <yggdrasil/util/latency_histogram.h>

namespace bpt::md_gateway::adapter {

// Parses Deribit JSON-RPC 2.0 WebSocket frames.
//
// Handled methods:
//   subscription → dispatches on channel prefix (quote / trades / book)
//   heartbeat    → sets test_request_pending flag for the adapter read loop
//
// Per-session state (reset on each reconnect via reset()):
//   last_change_id_ — tracks book change IDs for gap detection.
//   On a gap, the affected instrument is re-queued via subs_.requeue().
//
// test_request_pending is set from parse() and consumed by the adapter read
// loop via take_test_request(). Both are called sequentially on the IO thread
// so no locking is needed.
class DeribitParser : public IExchangeParser {
public:
    explicit DeribitParser(SubscriptionMap& subs) : subs_(subs) {}

    void parse(std::string_view payload,
               uint64_t recv_ns,
               messaging::IMdPublisher& pub,
               messaging::FundingRateCallback& on_funding_rate) override;

    // Clear per-session order book state. Called by the adapter before each (re)connect.
    void reset() override;

    // Returns true and clears the flag if parse() received a test_request heartbeat.
    // The adapter read loop calls this to know when to send a public/test response.
    [[nodiscard]] bool take_test_request() noexcept;

    // Remove gap-detection state for an unsubscribed instrument.
    void forget(const std::string& symbol);

    ygg::util::LatencyHistogram decode_lat_;

private:
    SubscriptionMap& subs_;  // non-const: requeue() is called on order book gaps
    std::unordered_map<std::string, uint64_t> last_change_id_;
    bool test_request_pending_{false};
    uint64_t tick_count_{0};
};

}  // namespace bpt::md_gateway::adapter

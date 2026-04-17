#pragma once

#include "md_gateway/messaging/funding_rate_publisher.h"
#include "md_gateway/messaging/i_md_publisher.h"

#include <cstdint>
#include <cstring>
#include <simdjson.h>
#include <string_view>
#include <vector>

namespace bpt::md_gateway::adapter {

// Interface for exchange-specific WebSocket message parsers.
//
// Each implementation:
//   - Holds a reference to the adapter's SubscriptionMap for symbol→id lookups.
//   - Parses one raw WebSocket frame and dispatches normalised events to the publisher.
//   - Must not allocate on the hot path except for multi-level order book vectors.
//   - May hold per-session state (e.g. sequence numbers for gap detection).
//     That state must be cleared in reset() before each (re)connect.
//
// Base provides:
//   json_parser_  — simdjson ondemand parser, reused across calls (pre-allocated internal buffer).
//   padded_buf_   — pre-allocated copy buffer satisfying simdjson's SIMDJSON_PADDING requirement.
//   pad(payload)  — copies payload into padded_buf_ and zero-fills the trailing padding bytes.
//                   Grows but never shrinks → amortised zero allocation after warmup.
class IExchangeParser {
public:
    virtual ~IExchangeParser() = default;

    // Parse one WebSocket frame and publish normalised events.
    // Called from the adapter's IO thread on every received message.
    virtual void parse(std::string_view payload,
                       uint64_t recv_ns,
                       messaging::IMdPublisher& pub,
                       messaging::FundingRateCallback& on_funding_rate) = 0;

    // Clear any per-session state. Called by the adapter before each (re)connect.
    virtual void reset() {}

protected:
    simdjson::ondemand::parser json_parser_;
    std::vector<char> padded_buf_;

    // Copy payload into padded_buf_, then zero-fill SIMDJSON_PADDING trailing bytes.
    // Must be called before each json_parser_.iterate() call.
    void pad(std::string_view payload) {
        const std::size_t needed = payload.size() + simdjson::SIMDJSON_PADDING;
        if (padded_buf_.size() < needed)
            padded_buf_.resize(needed);
        std::memcpy(padded_buf_.data(), payload.data(), payload.size());
        std::memset(padded_buf_.data() + payload.size(), 0, simdjson::SIMDJSON_PADDING);
    }
};

}  // namespace bpt::md_gateway::adapter

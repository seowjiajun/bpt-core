#pragma once

#include "md_gateway/adapter/common/i_exchange_parser.h"
#include "md_gateway/adapter/common/subscription_map.h"

#include <map>
#include <unordered_map>
#include <bpt_common/util/latency_histogram.h>

namespace bpt::md_gateway::adapter {

// Parses OKX WebSocket frames (JSON envelope with "arg" + "data").
//
// Handled channels:
//   bbo-tbt                   → handle_bbo  (tick-by-tick BBO)
//   books5 / books            → handle_book (top-N order book + derived BBO)
//   trades                    → handle_trades
//   funding-rate              → handle_funding_rate
//
// Channel dispatch uses a Channel enum + switch (compiler jump table) instead
// of a string if-else chain.  to_channel() maps the raw string to the enum once;
// the main parse() path never compares strings against channel names.
//
// Uses simdjson on-demand API with pre-allocated padded buffer (IExchangeParser base)
// for zero-allocation hot-path JSON parsing.  Decode latency (JSON parse + field
// extraction + Aeron offer) is recorded into decode_lat_ for Prometheus export.
class OkxParser : public IExchangeParser {
public:
    explicit OkxParser(const SubscriptionMap& subs) : subs_(subs) {}

    void parse(std::string_view payload,
               uint64_t recv_ns,
               messaging::IMdPublisher& pub,
               messaging::FundingRateCallback& on_funding_rate) override;

    bpt::common::util::LatencyHistogram decode_lat_;

private:
    // Channel is classified by a single char check on channel[0] plus the stored depth:
    //   channel[0] == 't'  → Trades        ("trades")
    //   channel[0] == 'f'  → FundingRate   ("funding-rate")
    //   else + depth == 0  → Bbo           ("bbo-tbt")
    //   else + depth >  0  → Book          ("books5" / "books")
    // No multi-char string comparison happens in the hot path.
    enum class Channel : uint8_t { Bbo, Book, Trades, FundingRate };

    void handle_bbo(simdjson::ondemand::object& entry,
                    uint64_t instrument_id,
                    uint64_t recv_ns,
                    uint64_t parse_start_ns,
                    messaging::IMdPublisher& pub);

    void handle_book(simdjson::ondemand::object& entry,
                     uint64_t instrument_id,
                     uint64_t recv_ns,
                     uint64_t parse_start_ns,
                     uint8_t depth,
                     bool is_snapshot,
                     messaging::IMdPublisher& pub);

    // Per-instrument local book state. OKX's `books` channel is
    // delta-based (snapshot then updates) — without tracking state we'd
    // publish the top of each delta slice as BBO, which is the
    // channel's *changes* rather than top-of-book. For `books5` the
    // every-message-is-a-snapshot contract holds, but we feed it
    // through the same state machine so the downstream view of the
    // ladder is consistent across depths.
    //
    // std::map ordered descending (bids) / ascending (asks) — best
    // price is always at begin(). Iteration cost is O(depth) per
    // publish; update cost is O(log n).
    struct BookState {
        std::map<double, double, std::greater<>> bids;
        std::map<double, double, std::less<>>    asks;
    };
    std::unordered_map<uint64_t, BookState> book_state_;

    void handle_trades(simdjson::ondemand::object& entry,
                       uint64_t instrument_id,
                       uint64_t recv_ns,
                       messaging::IMdPublisher& pub);

    void handle_funding_rate(simdjson::ondemand::object& entry,
                             uint64_t instrument_id,
                             uint64_t recv_ns,
                             messaging::FundingRateCallback& on_funding_rate);

    const SubscriptionMap& subs_;
    uint64_t tick_count_{0};
};

}  // namespace bpt::md_gateway::adapter

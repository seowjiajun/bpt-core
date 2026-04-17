#pragma once

#include <messages/TradeSide.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace bpt::md_gateway::md {

// Normalised market-data structs produced by exchange parsers and consumed by
// the SBE encoder (MdPublisher).  Plain value types — no heap allocation for
// BBO and Trade; OrderBook owns its level vectors (moved in from the parser).

struct MdBbo {
    uint64_t timestamp_ns{};
    uint64_t instrument_id{};
    double bid_price{};
    double bid_qty{};
    double ask_price{};
    double ask_qty{};
};

struct MdTrade {
    uint64_t timestamp_ns{};
    uint64_t instrument_id{};
    double price{};
    double qty{};
    bpt::messages::TradeSide::Value side{bpt::messages::TradeSide::BUY};
};

struct MdOrderBook {
    uint64_t timestamp_ns{};
    uint64_t instrument_id{};
    std::vector<std::pair<double, double>> bids;
    std::vector<std::pair<double, double>> asks;
};

}  // namespace bpt::md_gateway::md

#pragma once

#include <messages/TradeSide.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <bpt_common/util/inline_vec.h>

namespace bpt::md_gateway::md {

// Normalised market-data structs produced by exchange parsers and consumed by
// the SBE encoder (MdPublisher). Plain value types — no heap allocation
// anywhere on the publish path.

/// Maximum supported levels per side on the order-book domain type.
/// Mirrors the SBE schema cap; MdEncoder rejects books that exceed it.
inline constexpr std::size_t kMaxBookLevels = 20;

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
    /// Inline-storage levels — no malloc on the hot path. See
    /// bpt_common/util/inline_vec.h. Capacity matches MdEncoder's
    /// kMaxLevels; oversized inputs are rejected upstream by the
    /// adapter before they reach this struct.
    using Levels = bpt::common::util::InlineVec<std::pair<double, double>, kMaxBookLevels>;

    uint64_t timestamp_ns{};
    uint64_t instrument_id{};
    Levels bids;
    Levels asks;
};

}  // namespace bpt::md_gateway::md

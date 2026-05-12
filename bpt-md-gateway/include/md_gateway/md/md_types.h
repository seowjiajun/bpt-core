#pragma once

/// \file
/// \brief Normalised in-memory market-data types — the boundary between
///        venue-specific decoders and the SBE encoding/publishing layer.
///
/// Every adapter parses its native wire format (JSON, FIX, binary) into
/// these plain-value structs before handing them to ValidatingPublisher
/// → MdPublisher. Keeping the publish path on POD types means no heap
/// allocation between decode and Aeron, which is the contract the hot
/// path is designed around.

#include <messages/TradeSide.h>

#include <bpt_common/util/inline_vec.h>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace bpt::md_gateway::md {

/// \brief Maximum supported levels per side on MdOrderBook.
///
/// Mirrors the SBE schema cap. MdEncoder rejects books that exceed it,
/// and the InlineVec capacity in MdOrderBook is fixed to this value so
/// the two limits cannot drift apart.
inline constexpr std::size_t kMaxBookLevels = 20;

/// \brief Top-of-book quote (best bid + best ask).
struct MdBbo {
    uint64_t timestamp_ns{};   ///< venue or local recv timestamp (ns since epoch)
    uint64_t instrument_id{};  ///< canonical refdata instrument ID
    double bid_price{};
    double bid_qty{};
    double ask_price{};
    double ask_qty{};
};

/// \brief Single trade print.
struct MdTrade {
    uint64_t timestamp_ns{};   ///< venue or local recv timestamp (ns since epoch)
    uint64_t instrument_id{};  ///< canonical refdata instrument ID
    double price{};
    double qty{};
    bpt::messages::TradeSide::Value side{bpt::messages::TradeSide::BUY};  ///< aggressor side
};

/// \brief Snapshot or fully-merged delta of an L2 order book.
///
/// Bids are descending by price; asks are ascending. Adapters that
/// receive deltas merge them into a sorted ladder before populating
/// this struct — downstream code assumes monotonic ordering.
struct MdOrderBook {
    /// \brief Inline-storage levels — no malloc on the hot path.
    ///
    /// Capacity matches MdEncoder's kMaxLevels; oversized inputs are
    /// rejected upstream by the adapter before reaching this struct.
    using Levels = bpt::common::util::InlineVec<std::pair<double, double>, kMaxBookLevels>;

    uint64_t timestamp_ns{};
    uint64_t instrument_id{};
    Levels bids;
    Levels asks;
};

}  // namespace bpt::md_gateway::md

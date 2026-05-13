#pragma once

/// \file
/// \brief Top-of-book L2 snapshot record used by backtest replay.

#include <array>
#include <cstdint>
#include <string>

namespace bpt::backtester::data {

inline constexpr int kOrderBookDepth = 5;

struct OrderBookRecord {
    uint64_t timestamp_ns;
    std::string exchange;
    std::string symbol;

    /// Bids sorted descending (best bid first), asks ascending (best ask first).
    std::array<double, kOrderBookDepth> bid_px{};
    std::array<double, kOrderBookDepth> bid_sz{};
    std::array<double, kOrderBookDepth> ask_px{};
    std::array<double, kOrderBookDepth> ask_sz{};
};

}  // namespace bpt::backtester::data

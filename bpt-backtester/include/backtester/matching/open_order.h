#pragma once

/// \file
/// \brief Open-order and fill-report types used by the backtest matching engine.

#include <cstdint>
#include <string>

namespace bpt::backtester::matching {

/// \brief Order type.
///
/// Mirrors the three forms supported by the venues backtested against —
/// MARKET, LIMIT, and POST_ONLY. The matching engine uses the type to
/// decide whether to cross the book at submit (MARKET / crossing LIMIT),
/// rest as a resting limit (non-crossing LIMIT), or reject synchronously
/// when a POST_ONLY would cross.
enum class OrderType {
    MARKET,    ///< Always crosses the book on submission. Always TAKER.
    LIMIT,     ///< Rests if non-crossing; TAKER fill if it crosses at submit.
    POST_ONLY  ///< Must rest as MAKER. Rejected at submit if it would cross.
};

/// \brief Order side.
enum class OrderSide {
    BUY,  ///< Buying — sits on the bid side, fills when SELL prints at our price.
    SELL  ///< Selling — sits on the ask side, fills when BUY prints at our price.
};

/// \brief Identifies whether a fill consumed liquidity (TAKER) or provided it (MAKER).
///
/// Drives fee selection in ResultsCollector — venues typically charge taker
/// fees and either charge or rebate maker fees, so the two must be tracked
/// separately to predict P&L accurately.
///
/// Mapping:
///   - MARKET orders → TAKER (always cross at submit)
///   - Crossing-LIMIT orders → TAKER for the portion that crosses at submit
///   - Resting LIMIT consumed by an incoming trade print → MAKER
enum class LiquidityRole {
    MAKER,  ///< Order rested in the book and was consumed by an incoming print.
    TAKER   ///< Order crossed the book at submit, consuming resting liquidity.
};

/// \brief A live order tracked by the matching engine.
///
/// Created by callers (the simulated OrderGateway) and handed to
/// MatchingEngine::submit_order(). The engine mutates filled_qty,
/// queue_seeded, submitted_ts, and rejected as the order progresses
/// from submit through eventual fill or cancel.
struct OpenOrder {
    /// Engine-unique order identifier. Must be set by the caller; the
    /// engine uses it for cancel lookup and as the FillReport.order_id.
    std::string order_id;
    /// Caller-supplied client tag. Echoed back unchanged in every
    /// FillReport. Used by upstream code (OrderManager / strategy) to
    /// correlate fills with their originating strategy intent.
    std::string client_order_id;
    /// Wire-format exchange name (e.g. "BINANCE", "OKX", "HYPERLIQUID").
    std::string exchange;
    /// Venue-native symbol (e.g. "BTCUSDT", "BTC-USDT-SWAP", "BTC").
    std::string symbol;
    OrderType type{OrderType::LIMIT};
    OrderSide side{OrderSide::BUY};
    /// LIMIT / POST_ONLY price. Ignored for MARKET orders.
    double price{0.0};
    /// Original requested quantity. Never modified after submit.
    double quantity{0.0};
    /// Cumulative qty filled so far. Updated by the engine as fills happen.
    /// Order is fully filled when filled_qty >= quantity (within tolerance).
    double filled_qty{0.0};
    /// Engine-stamped simulated timestamp (ns) at which this order was
    /// accepted. Drives the SUBMIT_TO_MATCH leg of the latency model.
    uint64_t submitted_ts{0};

    /// True iff the matching engine successfully placed a slot for this
    /// order in the synthetic-L3 deque at submit time. When false the
    /// order falls back to the crossing-LIMIT path (the price was not
    /// visible in the L5 snapshot at submit, e.g. a new level between
    /// the bid and ask, or deeper than L5).
    bool queue_seeded{false};

    /// True iff the matching engine refused the order at submit time.
    /// Currently only set for POST_ONLY orders whose price would cross
    /// the current book at submit. The caller (venue's order server)
    /// inspects this to send the appropriate venue-format error back to
    /// the OrderGateway.
    bool rejected{false};
};

/// \brief Fill notification emitted by the matching engine.
///
/// Delivered via MatchingEngine::FillCallback after a fill happens (and
/// after the match-to-report latency leg has elapsed, when a latency
/// model is installed). A single OpenOrder may produce multiple
/// FillReports — once per partial-fill chunk — culminating in a final
/// report with is_fully_filled==true.
struct FillReport {
    /// Echoes OpenOrder.order_id.
    std::string order_id;
    /// Echoes OpenOrder.client_order_id.
    std::string client_order_id;
    std::string exchange;
    std::string symbol;
    /// Type of the originating OpenOrder.
    OrderType order_type{OrderType::LIMIT};
    /// Side of the originating OpenOrder.
    OrderSide side{OrderSide::BUY};
    /// Whether this specific fill chunk consumed liquidity or provided it.
    LiquidityRole liquidity_role{LiquidityRole::MAKER};
    /// Original requested quantity of the OpenOrder.
    double original_qty{0.0};
    /// LIMIT price of the originating order. 0 for MARKET.
    double order_price{0.0};
    /// Quantity of this fill chunk (may be less than original_qty for partial fills).
    double last_fill_qty{0.0};
    /// Price at which this chunk filled. For TAKER fills this is the
    /// resting counterparty's price (which can be better than the
    /// originating order's limit). For MAKER fills this is the resting
    /// LIMIT price.
    double last_fill_price{0.0};
    /// Sum of all last_fill_qty values emitted for this order so far,
    /// including this chunk.
    double cumulative_fill_qty{0.0};
    /// True iff this report fully closes the originating order (no
    /// further FillReports will be emitted for it).
    bool is_fully_filled{false};
    /// Engine-simulated timestamp (ns) when the fill matched.
    uint64_t simulation_ts{0};
};

}  // namespace bpt::backtester::matching

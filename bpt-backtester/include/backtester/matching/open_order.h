#pragma once

/// \file
/// \brief Open-order and fill-report types used by the backtest matching engine.

#include <cstdint>
#include <string>

namespace bpt::backtester::matching {

/// \brief Order type enum.
///
/// MARKET    — always crosses the book on submission (TAKER).
/// LIMIT     — rests in pending_ if non-crossing; fills as TAKER if it
///             crosses at submit (the crossing-LIMIT path).
/// POST_ONLY — must rest as MAKER. If it would cross at submit time,
///             the matching engine rejects it (mirrors HL Alo / OKX
///             post_only / Binance LIMIT_MAKER). Never appears as a
///             TAKER fill on results, by construction.
enum class OrderType { MARKET, LIMIT, POST_ONLY };
enum class OrderSide { BUY, SELL };

/// \brief Identifies whether a fill consumed liquidity (TAKER) or provided it (MAKER).
///
/// Drives fee selection in ResultsCollector — venues typically charge taker
/// fees and either charge or rebate maker fees, so the two must be tracked
/// separately to predict P&L accurately.
///
/// Mapping in MatchingEngine:
///   MARKET orders → TAKER (always cross the book on submission)
///   LIMIT orders that rest in `pending_` and fill on a later book update
///     → MAKER (the book moved to them; they were passive)
///
/// \note The matching engine does not currently distinguish a LIMIT
/// submitted at an already-crossing price (which a real exchange would
/// fill as TAKER) — that's a separate fidelity gap from the fee model.
/// All LIMIT fills today route through fill_crossing_limits and are
/// treated as MAKER.
enum class LiquidityRole { MAKER, TAKER };

struct OpenOrder {
    std::string order_id;
    std::string client_order_id;
    std::string exchange;
    std::string symbol;
    OrderType type{OrderType::LIMIT};
    OrderSide side{OrderSide::BUY};
    double price{0.0};  ///< LIMIT price; unused for MARKET.
    double quantity{0.0};
    double filled_qty{0.0};
    uint64_t submitted_ts{0};

    // ── Queue position tracking (LIMIT orders only) ─────────────────────
    //
    // Volume of resting orders ahead of us at our price level. Seeded at
    // submit_order() time from the current book snapshot's bid_sz/ask_sz
    // at our `price`. Drained by trade prints at our price (FIFO: prints
    // consume queue-ahead before they reach us). Once `queue_ahead`
    // reaches 0, subsequent prints fill us directly until the order is
    // fully filled.
    //
    // queue_seeded is false when we couldn't find our price in the book
    // at submit time (no L2 snapshot yet, or our price is deeper than
    // L5). Such orders fall back to the legacy fill_crossing_limits
    // path, which still over-fills — accepted limitation, logged once
    // per order if it matters.
    //
    // This model under-models cancellations from ahead of us (we have
    // no L3 visibility), so backtests are still mildly optimistic vs
    // live, but materially less so than the previous "fill 100% on any
    // cross" engine.
    double queue_ahead{0.0};
    bool queue_seeded{false};

    /// True iff the matching engine refused the order at submit time
    /// (currently only POST_ONLY orders that would cross). Returned in
    /// the OpenOrder MatchingEngine::submit_order(...) returns; the
    /// caller (each venue's order server) inspects it to send a
    /// venue-format error response back to the OGW.
    bool rejected{false};
};

struct FillReport {
    std::string order_id;
    std::string client_order_id;
    std::string exchange;
    std::string symbol;
    OrderType order_type{OrderType::LIMIT};
    OrderSide side{OrderSide::BUY};
    LiquidityRole liquidity_role{LiquidityRole::MAKER};
    double original_qty{0.0};
    double order_price{0.0};  ///< limit price of the original order.
    double last_fill_qty{0.0};
    double last_fill_price{0.0};
    double cumulative_fill_qty{0.0};
    bool is_fully_filled{false};
    uint64_t simulation_ts{0};
};

}  // namespace bpt::backtester::matching

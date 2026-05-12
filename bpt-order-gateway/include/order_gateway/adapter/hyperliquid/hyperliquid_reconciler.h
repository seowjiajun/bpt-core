#pragma once

/// \file
/// \brief Phantom-fill recovery for the Hyperliquid adapter.
///
/// **Scenario this closes**: adapter sends a signed action over WS →
/// bytes reach HL → HL accepts, possibly fills → the ack response does
/// NOT reach us (WS drops mid-flight, 5 s post_action timeout fires).
/// The adapter used to emit REJECTED to the strategy even though there
/// may be an orphan fill or a resting order on HL. The strategy's view
/// of position then diverges silently from the exchange.
///
/// **Recovery**: on post_action failure, instead of emitting REJECTED
/// immediately, push a Candidate to this reconciler. After a grace
/// period (default 3 s — long enough for late WS replay, short enough
/// not to stall the strategy's state machine) we call the injected
/// poller — typically REST `/info openOrders + /info userFills` — and
/// match the authoritative state back to the intent by `(asset_idx,
/// side, qty, price within 1 tick, time >= sent_ns - 1 s)`.
///
/// HL does not echo a cloid in any response we see. The correlator is
/// the original order shape itself. This makes multi-match possible:
/// if the strategy had two identical-shape intents in flight when the
/// drop happened, we can't safely pick one — we emit REJECTED for both
/// and log an ERROR, relying on the position reconciler (strategy-side
/// AccountSnapshot diff) to surface the mismatch.

#include "order_gateway/adapter/common/i_order_adapter.h"

#include <atomic>
#include <boost/json.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bpt::order_gateway::adapter::hyperliquid {

class HyperliquidReconciler {
public:
    /// \brief Everything the adapter knows about an order whose post_action threw or timed out.
    ///
    /// `exchange_symbol` is the HL coin string (e.g. "BTC", "ETH") — used
    /// for coin-side matching against openOrders/userFills entries.
    /// `sent_ns` is the wall-clock nanosecond at which we issued
    /// post_action; the user-fills match uses a time-window that starts
    /// 1 s before this.
    struct Candidate {
        uint64_t client_order_id;
        uint64_t instrument_id;
        bpt::messages::OrderSide::Value side;
        bpt::messages::OrderType::Value order_type;
        int64_t price_e8;
        uint64_t quantity_e8;
        std::string exchange_symbol;
        uint64_t sent_ns;
    };

    enum class MatchKind { None, OpenOrder, UserFill, Ambiguous };

    /// \brief The authoritative state the match resolved to.
    ///
    /// For OpenOrder, only `exch_oid` is populated — the order is
    /// resting, strategy learns via ACKED. For UserFill we additionally
    /// carry `fill_price_e8` / `fill_fee_e8` / `fill_qty_e8` /
    /// `fill_time_ns` so the emit path can produce a FILLED ExecEvent
    /// identical to what the WS userFills stream would have.
    struct MatchResult {
        MatchKind kind{MatchKind::None};
        uint64_t exch_oid{0};
        int64_t fill_price_e8{0};
        int64_t fill_fee_e8{0};
        uint64_t fill_qty_e8{0};
        uint64_t fill_time_ns{0};
    };

    /// \brief Pure match logic — exposed as a static so unit tests can drive it directly.
    ///
    /// Returns Ambiguous if more than one open_order OR more than one
    /// user_fill matches the candidate. Open-order matches take priority
    /// over user-fill matches only when exactly-one of each class, in
    /// which case we prefer UserFill (more specific terminal state).
    /// `price_tick_e8` is the price quantum for the asset; a match
    /// allows price drift up to ±price_tick_e8 to accommodate HL's tick
    /// rounding.
    static MatchResult try_match(const Candidate& c,
                                 const boost::json::array& open_orders,
                                 const boost::json::array& user_fills,
                                 int64_t price_tick_e8);

    /// \brief Returns `(open_orders, user_fills)` as returned by HL's `/info` endpoint.
    ///
    /// Both are arrays; empty on failure (the caller's job to log and
    /// back off — reconciler treats empty as "no match → REJECTED").
    using Poller = std::function<std::pair<boost::json::array, boost::json::array>()>;

    /// \brief Callback invoked exactly once per candidate when reconciliation completes.
    ///
    /// Runs on the reconciler's worker thread.
    using OnTerminal = std::function<void(const Candidate&, const MatchResult&)>;

    HyperliquidReconciler(Poller poller,
                          OnTerminal on_terminal,
                          std::chrono::milliseconds grace_period,
                          int64_t price_tick_e8);
    ~HyperliquidReconciler();

    HyperliquidReconciler(const HyperliquidReconciler&) = delete;
    HyperliquidReconciler& operator=(const HyperliquidReconciler&) = delete;

    /// \brief Enqueue a candidate.
    ///
    /// Returns immediately — reconciliation runs on the worker thread
    /// after the grace period.
    void reconcile_async(Candidate c);

private:
    void worker_loop();

    Poller poller_;
    OnTerminal on_terminal_;
    std::chrono::milliseconds grace_period_;
    int64_t price_tick_e8_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Candidate> queue_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace bpt::order_gateway::adapter::hyperliquid

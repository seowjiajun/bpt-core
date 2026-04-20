#include "order_gateway/adapter/hyperliquid/hyperliquid_reconciler.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <bpt_common/logging.h>

namespace bpt::order_gateway::adapter::hyperliquid {

namespace json = boost::json;

namespace {

constexpr double kScale = 1e8;

// HL's userFills "side" is "B" (buy) or "A" (ask/sell).
bool side_matches(const boost::json::object& o,
                  bpt::messages::OrderSide::Value expected) {
    auto it = o.find("side");
    if (it == o.end() || !it->value().is_string()) return false;
    const std::string_view s(it->value().as_string());
    const bool is_buy = (s == "B");
    return (is_buy && expected == bpt::messages::OrderSide::BUY) ||
           (!is_buy && expected == bpt::messages::OrderSide::SELL);
}

std::string coin_of(const boost::json::object& o) {
    auto it = o.find("coin");
    if (it == o.end() || !it->value().is_string()) return {};
    return std::string(it->value().as_string());
}

// "72198.0" → 72198.0; silently returns 0.0 on malformed input (caller
// then fails the range check).
double parse_decimal_string(const boost::json::object& o, const char* key) {
    auto it = o.find(key);
    if (it == o.end() || !it->value().is_string()) return 0.0;
    try {
        return std::stod(std::string(it->value().as_string()));
    } catch (...) {
        return 0.0;
    }
}

int64_t to_e8(double v) { return static_cast<int64_t>(std::round(v * kScale)); }

// HL's openOrder shape (/info openOrders):
//   { coin:"BTC", side:"B"|"A", sz:"0.0001", limitPx:"72198.0",
//     oid:123, timestamp:1700000000000, origSz:"0.0001", ... }
// Returns true if the open_order shape matches the candidate exactly on
// (coin, side, qty_e8) and within ±price_tick_e8 on price.
bool open_order_matches(const boost::json::object& o,
                        const HyperliquidReconciler::Candidate& c,
                        int64_t price_tick_e8) {
    if (coin_of(o) != c.exchange_symbol) return false;
    if (!side_matches(o, c.side)) return false;

    const int64_t sz_e8 = to_e8(parse_decimal_string(o, "sz"));
    if (sz_e8 != static_cast<int64_t>(c.quantity_e8)) return false;

    const int64_t px_e8 = to_e8(parse_decimal_string(o, "limitPx"));
    return std::llabs(px_e8 - c.price_e8) <= price_tick_e8;
}

// HL's userFills shape (/info userFills):
//   { coin:"BTC", side:"B"|"A", sz:"0.0001", px:"72198.0",
//     fee:"0.0005", time:1700000000000, oid:123, ... }
// A single userFills entry is one slice. We only use this to determine
// that the order was fulfilled at all — partial multi-slice fills will
// continue to route via the WS userFills stream once we register the
// oid mapping.
bool user_fill_matches(const boost::json::object& o,
                       const HyperliquidReconciler::Candidate& c,
                       int64_t price_tick_e8) {
    if (coin_of(o) != c.exchange_symbol) return false;
    if (!side_matches(o, c.side)) return false;

    // 1-second clock-skew window: HL timestamps are the exchange's
    // clock, sent_ns is ours.
    auto time_it = o.find("time");
    if (time_it == o.end() || !time_it->value().is_int64()) return false;
    const uint64_t time_ns = static_cast<uint64_t>(time_it->value().as_int64()) * 1'000'000ULL;
    if (time_ns + 1'000'000'000ULL < c.sent_ns) return false;

    const int64_t px_e8 = to_e8(parse_decimal_string(o, "px"));
    if (std::llabs(px_e8 - c.price_e8) > price_tick_e8) return false;

    // Size match: an aggressive fill may be less than the original qty
    // (IOC partial). Accept any fill whose size is <= quantity_e8 AND
    // > 0 — the registered oid mapping will pick up trailing slices.
    const int64_t sz_e8 = to_e8(parse_decimal_string(o, "sz"));
    return sz_e8 > 0 && sz_e8 <= static_cast<int64_t>(c.quantity_e8);
}

uint64_t get_oid(const boost::json::object& o) {
    auto it = o.find("oid");
    if (it == o.end()) return 0;
    if (it->value().is_int64()) return static_cast<uint64_t>(it->value().as_int64());
    if (it->value().is_uint64()) return it->value().as_uint64();
    return 0;
}

}  // namespace

HyperliquidReconciler::MatchResult
HyperliquidReconciler::try_match(const Candidate& c,
                                 const boost::json::array& open_orders,
                                 const boost::json::array& user_fills,
                                 int64_t price_tick_e8) {
    // Count matches in each class. Ambiguity in EITHER class is fatal:
    // we cannot guess which identical intent this fill belongs to.
    const boost::json::object* fill_match = nullptr;
    int fill_match_count = 0;
    for (const auto& v : user_fills) {
        if (!v.is_object()) continue;
        const auto& o = v.as_object();
        if (user_fill_matches(o, c, price_tick_e8)) {
            fill_match = &o;
            ++fill_match_count;
        }
    }

    const boost::json::object* order_match = nullptr;
    int order_match_count = 0;
    for (const auto& v : open_orders) {
        if (!v.is_object()) continue;
        const auto& o = v.as_object();
        if (open_order_matches(o, c, price_tick_e8)) {
            order_match = &o;
            ++order_match_count;
        }
    }

    if (fill_match_count > 1 || order_match_count > 1)
        return {MatchKind::Ambiguous, 0, 0, 0, 0, 0};

    // Fill takes priority: if both a resting order and a fill match,
    // the fill is the later, more-specific state. (A resting order is
    // what becomes a fill — we skip the intermediate ACKED to avoid
    // having the strategy briefly see a "resting" state that's already
    // terminal on HL.)
    if (fill_match_count == 1) {
        MatchResult r;
        r.kind = MatchKind::UserFill;
        r.exch_oid = get_oid(*fill_match);
        r.fill_price_e8 = to_e8(parse_decimal_string(*fill_match, "px"));
        r.fill_fee_e8 = to_e8(parse_decimal_string(*fill_match, "fee"));
        r.fill_qty_e8 = static_cast<uint64_t>(to_e8(parse_decimal_string(*fill_match, "sz")));
        if (auto it = fill_match->find("time");
            it != fill_match->end() && it->value().is_int64())
            r.fill_time_ns = static_cast<uint64_t>(it->value().as_int64()) * 1'000'000ULL;
        return r;
    }

    if (order_match_count == 1) {
        MatchResult r;
        r.kind = MatchKind::OpenOrder;
        r.exch_oid = get_oid(*order_match);
        return r;
    }

    return {MatchKind::None, 0, 0, 0, 0, 0};
}

HyperliquidReconciler::HyperliquidReconciler(Poller poller,
                                             OnTerminal on_terminal,
                                             std::chrono::milliseconds grace_period,
                                             int64_t price_tick_e8)
    : poller_(std::move(poller)),
      on_terminal_(std::move(on_terminal)),
      grace_period_(grace_period),
      price_tick_e8_(price_tick_e8),
      worker_([this] { worker_loop(); }) {}

HyperliquidReconciler::~HyperliquidReconciler() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void HyperliquidReconciler::reconcile_async(Candidate c) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        queue_.push_back(std::move(c));
    }
    cv_.notify_one();
}

void HyperliquidReconciler::worker_loop() {
    while (!stop_.load(std::memory_order_acquire)) {
        Candidate c;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !queue_.empty();
            });
            if (stop_.load(std::memory_order_acquire)) return;
            c = std::move(queue_.front());
            queue_.pop_front();
        }

        // Grace period — let a late WS response / userFills replay land
        // first. If it did, the adapter has already registered the oid
        // mapping; we'll still run the REST match below and emit, but
        // the adapter's callback is expected to detect the race (oid
        // already registered) and skip the duplicate emit.
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait_for(lock, grace_period_,
                         [this] { return stop_.load(std::memory_order_acquire); });
            if (stop_.load(std::memory_order_acquire)) return;
        }

        MatchResult result;
        try {
            auto [open_orders, user_fills] = poller_();
            result = try_match(c, open_orders, user_fills, price_tick_e8_);
        } catch (const std::exception& e) {
            bpt::common::log::warn("HL reconciler: poll failed for client_id={}: {}",
                           c.client_order_id, e.what());
            result = {MatchKind::None, 0, 0, 0, 0, 0};
        }

        if (result.kind == MatchKind::Ambiguous) {
            bpt::common::log::error(
                "HL reconciler: AMBIGUOUS — multiple candidates match "
                "client_id={} coin={} side={} qty_e8={} px_e8={}. Emitting REJECTED; "
                "strategy position reconciler will surface any divergence.",
                c.client_order_id, c.exchange_symbol,
                c.side == bpt::messages::OrderSide::BUY ? "BUY" : "SELL",
                c.quantity_e8, c.price_e8);
        }

        on_terminal_(c, result);
    }
}

}  // namespace bpt::order_gateway::adapter::hyperliquid

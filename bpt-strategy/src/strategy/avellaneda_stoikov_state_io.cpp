// AS console JSON + warm-start save/load. Pure serialisation.

#include "strategy/strategy/avellaneda_stoikov_strategy.h"

#include "strategy/clock/sim_clock.h"

#include <algorithm>
#include <bpt_common/logging.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <system_error>

namespace bpt::strategy::strategy {

namespace {
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("AS");
    return l;
}

// Bump on breaking format changes.
constexpr int kWarmStartSchemaVersion = 1;
}  // namespace

// ── Strategy state for console ────────────────────────────────────────────

std::string AvellanedaStoikovStrategy::get_strategy_state_json() {
    // Single instrument for now — take the first entry in state_.
    if (state_.empty())
        return {};

    const auto& [instrument_id, st] = *state_.begin();
    const double net_qty = static_cast<double>(positions_.net_qty(instrument_id, st.exchange_id)) / 1e8;

    // Compute current quotes to get reservation and half-spread.
    double bid_quote = 0, ask_quote = 0;
    bool quotes_valid = false;
    if (st.last_mid > 0 && st.ewma_ticks >= vol_warmup_ticks_) {
        quotes_valid = compute_quotes(st, instrument_id, net_qty, st.last_mid, st.last_tick_ns, bid_quote, ask_quote);
    }

    const double half_spread = quotes_valid ? (ask_quote - bid_quote) / 2.0 : 0.0;
    const double reservation = quotes_valid ? (bid_quote + ask_quote) / 2.0 : st.last_mid;
    const double reservation_offset_bps = st.last_mid > 0 ? (reservation - st.last_mid) / st.last_mid * 1e4 : 0.0;

    // Suppression snapshot shared with maybe_requote — single source of
    // truth so the console badge can't disagree with the actual
    // runtime decision. Queue suppression is only meaningful when
    // quotes_valid (pre-warmup returns fp=1); compute_suppression
    // computes it unconditionally but the projected prices below are
    // still defaulted from the struct, which is correct since st.book
    // wouldn't be ready during warmup anyway.
    const SuppressionState supp = compute_suppression(st, net_qty, bid_quote, ask_quote);
    const double drift_bps = std::abs(st.ewma_drift) * 1e4;  // used in driftBps JSON field below
    const double projected_fp_bid = supp.fp_bid;
    const double projected_fp_ask = supp.fp_ask;

    // queue_ahead for any live resting orders — the ACTUAL tracked queue,
    // not the projected one. Used by the console to show how buried the
    // current resting orders are.
    double bid_queue_ahead = 0.0;
    double ask_queue_ahead = 0.0;
    double bid_fill_prob = 0.0;
    double ask_fill_prob = 0.0;
    if (st.bid_order_id != 0) {
        if (const auto* e = st.queue.lookup(st.bid_order_id)) {
            bid_queue_ahead = e->queue_ahead;
            bid_fill_prob = st.queue.fill_probability(st.bid_order_id);
        }
    }
    if (st.ask_order_id != 0) {
        if (const auto* e = st.queue.lookup(st.ask_order_id)) {
            ask_queue_ahead = e->queue_ahead;
            ask_fill_prob = st.queue.fill_probability(st.ask_order_id);
        }
    }

    nlohmann::json j;
    j["type"] = "strategyState";
    // Discriminator for the console's panel registry. Every strategy
    // that implements get_strategy_state_json() must set `kind`; the
    // frontend picks the matching panel component (panels/index.ts).
    j["kind"] = "AS";
    j["symbol"] = st.symbol;
    j["exchange"] = st.exchange;

    // Model parameters (live values, not config)
    j["drift"] = st.ewma_drift;
    j["driftBps"] = drift_bps;
    j["slowDriftBps"] = st.slow_drift_bps;
    j["slowDriftSuppressBps"] = slow_drift_suppress_bps_;
    j["sigma2"] = st.ewma_var;
    j["kappa"] = (st.kappa_ticks >= kappa_warmup_ticks_) ? std::max(kappa_min_, st.ewma_kappa) : kappa_;
    j["kappaLive"] = st.kappa_ticks >= kappa_warmup_ticks_;

    // Regime
    j["regime"] = st.regime.regime_name();
    j["hurst"] = st.regime.hurst();
    j["gammaBase"] = gamma_;
    const double gpnl_mult = gamma_pnl_mult(st);
    j["gammaEffective"] = gamma_ * st.regime.gamma_multiplier() * gpnl_mult;
    j["gammaMultiplier"] = st.regime.gamma_multiplier();
    j["gammaPnlMultiplier"] = gpnl_mult;
    j["gammaPnlWindow"] = static_cast<int>(gamma_pnl_window_n_);
    j["gammaPnlRecentSum"] = [&]() {
        double s = 0.0;
        for (double r : st.recent_rpnl)
            s += r;
        return s;
    }();

    // Quotes
    j["mid"] = st.last_mid;
    j["reservation"] = reservation;
    j["reservationOffsetBps"] = reservation_offset_bps;
    j["halfSpread"] = half_spread;
    j["halfSpreadBps"] = st.last_mid > 0 ? half_spread / st.last_mid * 1e4 : 0;

    // Inventory — report the EFFECTIVE cap (adaptive when configured),
    // not the static fallback, so the console inventoryPct gauge
    // tracks the same threshold the strategy is actually enforcing.
    const double max_inv = effective_max_inventory(st);
    j["inventory"] = net_qty;
    j["maxInventory"] = max_inv;
    j["inventoryPct"] = max_inv > 0 ? std::abs(net_qty) / max_inv * 100.0 : 0;

    // Suppression state per side — priority ladder lives on the
    // SuppressionState struct (vol_gate → inventory → drift → trend →
    // tox → queue). Both the boolean and reason string come from the
    // same struct so they can never drift.
    j["bidSuppressed"] = supp.bid_suppressed();
    j["bidSuppressReason"] = std::string(supp.bid_reason());
    j["askSuppressed"] = supp.ask_suppressed();
    j["askSuppressReason"] = std::string(supp.ask_reason());

    // Vol gate
    j["volGateHalted"] = supp.vol_halted;
    j["volGateTrips"] = st.vol_gate.trips_total();

    // Orders
    j["bidOrderLive"] = st.bid_order_id != 0;
    j["askOrderLive"] = st.ask_order_id != 0;
    j["bidPrice"] = st.last_bid_price;
    j["askPrice"] = st.last_ask_price;

    // Warmup
    j["volTicks"] = st.ewma_ticks;
    j["volWarmup"] = vol_warmup_ticks_;
    j["warmedUp"] = st.ewma_ticks >= vol_warmup_ticks_;

    // Queue state — actual (for resting orders) and projected (for the
    // quote the strategy would place on the next tick).
    j["bookBidLevels"] = st.book.n_bid_levels();
    j["bookAskLevels"] = st.book.n_ask_levels();
    j["bidQueueAhead"] = bid_queue_ahead;
    j["askQueueAhead"] = ask_queue_ahead;
    j["bidFillProb"] = bid_fill_prob;
    j["askFillProb"] = ask_fill_prob;
    j["bidProjectedFillProb"] = projected_fp_bid;
    j["askProjectedFillProb"] = projected_fp_ask;
    j["queueSuppressMin"] = queue_suppress_fill_prob_min_;

    // Market best bid/ask — cached by on_bbo. Preferred over st.book
    // because this strategy runs with order_book_depth=0 (no L2 ladder
    // consumption); st.book.ready() would always return false here.
    j["marketBid"] = st.last_market_bid;
    j["marketAsk"] = st.last_market_ask;

    return j.dump();
}

// ── Warm-start state ────────────────────────────────────────────────────────

void AvellanedaStoikovStrategy::save_state(const std::string& path) {
    if (path.empty())
        return;

    try {
        nlohmann::json root;
        root["version"] = kWarmStartSchemaVersion;
        root["saved_at_ns"] = bpt::strategy::clock::SimClock::now_ns();

        nlohmann::json instruments = nlohmann::json::array();
        for (const auto& [instrument_id, st] : state_) {
            nlohmann::json j;
            j["instrument_id"] = instrument_id;
            j["symbol"] = st.symbol;
            j["exchange"] = st.exchange;
            j["ewma_var"] = st.ewma_var;
            j["ewma_ticks"] = st.ewma_ticks;
            j["last_mid"] = st.last_mid;
            j["last_tick_ns"] = st.last_tick_ns;
            j["ewma_drift"] = st.ewma_drift;
            j["ewma_kappa"] = st.ewma_kappa;
            j["kappa_ticks"] = st.kappa_ticks;
            j["last_trade_ns"] = st.last_trade_ns;

            const auto rs = st.regime.snapshot_state();
            nlohmann::json r;
            r["regime"] = static_cast<int>(rs.regime);
            r["hurst"] = rs.hurst;
            r["last_mid"] = rs.last_mid;
            r["returns"] = rs.returns;
            r["tick_count"] = rs.tick_count;
            j["regime"] = std::move(r);

            instruments.push_back(std::move(j));
        }
        root["instruments"] = std::move(instruments);

        // Atomic write: tmp + rename so a crash mid-serialise doesn't
        // leave a half-written file that the next boot would load.
        std::filesystem::path p(path);
        std::filesystem::create_directories(p.parent_path());
        const std::filesystem::path tmp = p.string() + ".tmp";
        {
            std::ofstream ofs(tmp);
            if (!ofs) {
                bpt::common::log::error(kLog(), "warm-start save: cannot open {} for write", tmp.string());
                return;
            }
            ofs << root.dump(2);
        }
        std::filesystem::rename(tmp, p);
        bpt::common::log::info(kLog(), "warm-start saved {} instrument(s) to {}", state_.size(), p.string());
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "warm-start save failed: {}", e.what());
    }
}

void AvellanedaStoikovStrategy::load_state(const std::string& path, uint64_t max_age_s) {
    if (path.empty())
        return;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        bpt::common::log::info(kLog(), "warm-start: no prior state at {} — cold start", path);
        return;
    }

    try {
        std::ifstream ifs(path);
        nlohmann::json root = nlohmann::json::parse(ifs);

        const int version = root.value("version", 0);
        if (version != kWarmStartSchemaVersion) {
            bpt::common::log::warn(kLog(),
                                   "warm-start: schema mismatch (file={} expected={}) — cold start",
                                   version,
                                   kWarmStartSchemaVersion);
            return;
        }

        const uint64_t saved_at_ns = root.value<uint64_t>("saved_at_ns", 0);
        const uint64_t now_ns = bpt::strategy::clock::SimClock::now_ns();
        const uint64_t age_ns = (now_ns > saved_at_ns) ? (now_ns - saved_at_ns) : 0;
        const uint64_t max_age_ns = max_age_s * 1'000'000'000ULL;
        if (age_ns > max_age_ns) {
            bpt::common::log::warn(kLog(),
                                   "warm-start: saved state is {}s old (max {}s) — cold start",
                                   age_ns / 1'000'000'000ULL,
                                   max_age_s);
            return;
        }

        int restored = 0;
        int skipped = 0;
        for (const auto& j : root.value("instruments", nlohmann::json::array())) {
            const uint64_t instrument_id = j.value<uint64_t>("instrument_id", 0);
            auto it = state_.find(instrument_id);
            if (it == state_.end()) {
                ++skipped;  // instrument not in current universe
                continue;
            }
            auto& st = it->second;

            // Sanity: symbol + exchange must still match so a refdata
            // reshuffle doesn't silently graft OKX state onto a Binance
            // instrument that happens to have inherited the same id.
            const std::string saved_sym = j.value("symbol", "");
            const std::string saved_ex = j.value("exchange", "");
            if (saved_sym != st.symbol || saved_ex != st.exchange) {
                bpt::common::log::warn(kLog(),
                                       "warm-start: instrument_id={} symbol/exchange mismatch "
                                       "(saved '{}'/'{}' vs current '{}'/'{}') — skipping",
                                       instrument_id,
                                       saved_sym,
                                       saved_ex,
                                       st.symbol,
                                       st.exchange);
                ++skipped;
                continue;
            }

            st.ewma_var = j.value("ewma_var", 0.0);
            st.ewma_ticks = j.value<std::size_t>("ewma_ticks", 0);
            st.last_mid = j.value("last_mid", 0.0);
            st.last_tick_ns = j.value<uint64_t>("last_tick_ns", 0);
            st.ewma_drift = j.value("ewma_drift", 0.0);
            st.ewma_kappa = j.value("ewma_kappa", 0.0);
            st.kappa_ticks = j.value<std::size_t>("kappa_ticks", 0);
            st.last_trade_ns = j.value<uint64_t>("last_trade_ns", 0);

            if (auto r = j.find("regime"); r != j.end()) {
                RegimeDetector::StateSnapshot snap;
                snap.regime = static_cast<RegimeDetector::Regime>(r->value("regime", 0));
                snap.hurst = r->value("hurst", 0.5);
                snap.last_mid = r->value("last_mid", 0.0);
                snap.tick_count = r->value<std::size_t>("tick_count", 0);
                snap.returns = r->value("returns", std::vector<double>{});
                st.regime.restore_state(snap);
            }

            ++restored;
        }
        bpt::common::log::info(kLog(),
                               "warm-start: restored {} instrument(s) from {} (skipped {}, age {}s)",
                               restored,
                               path,
                               skipped,
                               age_ns / 1'000'000'000ULL);
    } catch (const std::exception& e) {
        bpt::common::log::error(kLog(), "warm-start load failed: {} — falling back to cold start", e.what());
    }
}

}  // namespace bpt::strategy::strategy

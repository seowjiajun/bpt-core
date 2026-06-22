// AS console JSON + warm-start save/load. Pure serialisation.

#include "strategy/clock/sim_clock.h"
#include "strategy/strategy/avellaneda_stoikov_strategy.h"

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

std::string AvellanedaStoikovStrategy::get_strategy_state_json() {
    if (state_.empty())
        return {};

    const auto& [instrument_id, st] = *state_.begin();
    const double net_qty = static_cast<double>(positions_.net_qty(instrument_id, st.exchange_id)) / 1e8;

    const auto& pc = pricer_.config();
    const auto fee_entry = refdata_.fee_cache().get(st.exchange_id, st.instrument_id, st.last_tick_ns);
    const double fee_hs = fee_entry ? (static_cast<double>(fee_entry->maker_bps) / 10000.0) * st.last_mid : 0.0;

    const auto quotes =
        (st.last_mid > 0 && st.ewma_var.count() >= pc.vol_warmup_ticks)
            ? pricer_.evaluate(st, BboContext{.net_qty = net_qty, .mid = st.last_mid, .ts_ns = st.last_tick_ns},
                               fee_hs)
            : std::nullopt;

    const double bid_quote = quotes ? quotes->bid : 0.0;
    const double ask_quote = quotes ? quotes->ask : 0.0;
    const double half_spread = quotes ? (ask_quote - bid_quote) / 2.0 : 0.0;
    const double reservation = quotes ? (bid_quote + ask_quote) / 2.0 : st.last_mid;
    const double reservation_offset_bps = st.last_mid > 0 ? (reservation - st.last_mid) / st.last_mid * 1e4 : 0.0;

    // Same supp_policy_ call as maybe_requote — badge can't disagree with the runtime decision.
    const SuppressionState supp =
        supp_policy_.evaluate(st, net_qty, bid_quote, ask_quote,
                              sizer_.effective_max_inventory(st.last_mid, last_equity_e8_),
                              sizer_.effective_qty(st.last_mid, st.lot_size, last_equity_e8_, st.exchange));
    const double drift_bps = std::abs(st.ewma_drift.value()) * 1e4;  // used in driftBps JSON field below
    const double projected_fp_bid = supp.fp_bid;
    const double projected_fp_ask = supp.fp_ask;

    double bid_queue_ahead = 0.0;
    double ask_queue_ahead = 0.0;
    double bid_fill_prob = 0.0;
    double ask_fill_prob = 0.0;
    if (st.h_bid.valid()) {
        const uint64_t oid = st.h_bid.order_id();
        if (const auto* e = st.queue.lookup(oid)) {
            bid_queue_ahead = e->queue_ahead;
            bid_fill_prob = st.queue.fill_probability(oid);
        }
    }
    if (st.h_ask.valid()) {
        const uint64_t oid = st.h_ask.order_id();
        if (const auto* e = st.queue.lookup(oid)) {
            ask_queue_ahead = e->queue_ahead;
            ask_fill_prob = st.queue.fill_probability(oid);
        }
    }

    nlohmann::json j;
    j["type"] = "strategyState";
    j["kind"] = "AS";
    j["symbol"] = st.symbol;
    j["exchange"] = st.exchange;

    j["drift"] = st.ewma_drift.value();
    j["driftBps"] = drift_bps;
    j["slowDriftBps"] = st.slow_drift_bps;
    j["slowDriftSuppressBps"] = supp_policy_.config().slow_drift_suppress_bps;
    j["sigma2"] = st.ewma_var.value();
    j["kappa"] = (st.ewma_kappa.count() >= pc.kappa_warmup_ticks) ? std::max(pc.kappa_min, st.ewma_kappa.value()) : pc.kappa;
    j["kappaLive"] = st.ewma_kappa.count() >= pc.kappa_warmup_ticks;

    j["regime"] = st.regime.regime_name();
    j["hurst"] = st.regime.hurst();
    j["gammaBase"] = pc.gamma;
    const double gpnl_mult = pricer_.gamma_pnl_mult(st);
    j["gammaEffective"] = pc.gamma * st.regime.gamma_multiplier() * gpnl_mult;
    j["gammaMultiplier"] = st.regime.gamma_multiplier();
    j["gammaPnlMultiplier"] = gpnl_mult;
    j["gammaPnlWindow"] = static_cast<int>(pc.gamma_pnl_window_n);
    j["gammaPnlRecentSum"] = [&]() {
        double s = 0.0;
        for (double r : st.recent_rpnl)
            s += r;
        return s;
    }();

    j["mid"] = st.last_mid;
    j["reservation"] = reservation;
    j["reservationOffsetBps"] = reservation_offset_bps;
    j["halfSpread"] = half_spread;
    j["halfSpreadBps"] = st.last_mid > 0 ? half_spread / st.last_mid * 1e4 : 0;

    // Effective cap (adaptive when fraction configured) so inventoryPct matches runtime enforcement.
    const double max_inv = sizer_.effective_max_inventory(st.last_mid, last_equity_e8_);
    j["inventory"] = net_qty;
    j["maxInventory"] = max_inv;
    j["inventoryPct"] = max_inv > 0 ? std::abs(net_qty) / max_inv * 100.0 : 0;

    j["bidSuppressed"] = supp.bid_suppressed();
    j["bidSuppressReason"] = std::string(supp.bid_reason());
    j["askSuppressed"] = supp.ask_suppressed();
    j["askSuppressReason"] = std::string(supp.ask_reason());

    j["volGateHalted"] = supp.vol_halted;
    j["volGateTrips"] = st.vol_gate.trips_total();

    j["bidOrderLive"] = st.h_bid.live();
    j["askOrderLive"] = st.h_ask.live();
    j["bidPrice"] = st.last_bid_price;
    j["askPrice"] = st.last_ask_price;

    j["volTicks"] = st.ewma_var.count();
    j["volWarmup"] = pc.vol_warmup_ticks;
    j["warmedUp"] = st.ewma_var.count() >= pc.vol_warmup_ticks;

    j["bookBidLevels"] = st.book.n_bid_levels();
    j["bookAskLevels"] = st.book.n_ask_levels();
    j["bidQueueAhead"] = bid_queue_ahead;
    j["askQueueAhead"] = ask_queue_ahead;
    j["bidFillProb"] = bid_fill_prob;
    j["askFillProb"] = ask_fill_prob;
    j["bidProjectedFillProb"] = projected_fp_bid;
    j["askProjectedFillProb"] = projected_fp_ask;
    j["queueSuppressMin"] = supp_policy_.config().queue_suppress_fill_prob_min;

    // Cached BBO — book may not be ready when order_book_depth=0.
    j["marketBid"] = st.last_market_bid;
    j["marketAsk"] = st.last_market_ask;

    return j.dump();
}

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
            j["ewma_var"] = st.ewma_var.value();
            j["ewma_ticks"] = st.ewma_var.count();
            j["last_mid"] = st.last_mid;
            j["last_tick_ns"] = st.last_tick_ns;
            j["ewma_drift"] = st.ewma_drift.value();
            j["drift_ticks"] = st.ewma_drift.count();
            j["ewma_kappa"] = st.ewma_kappa.value();
            j["kappa_ticks"] = st.ewma_kappa.count();
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

            const double last_mid = j.value("last_mid", 0.0);
            const uint64_t last_tick_ns = j.value<uint64_t>("last_tick_ns", 0);
            const uint64_t last_trade_ns = j.value<uint64_t>("last_trade_ns", 0);

            st.ewma_var.restore({
                .value = j.value("ewma_var", 0.0),
                .count = j.value<std::size_t>("ewma_ticks", 0),
                .last_mid = last_mid,
                .last_ns = last_tick_ns,
            });
            st.ewma_drift.restore({
                .value = j.value("ewma_drift", 0.0),
                .count = j.value<std::size_t>("drift_ticks", 0),
                .last_mid = last_mid,
                .last_ns = last_tick_ns,
            });
            st.ewma_kappa.restore({
                .value = j.value("ewma_kappa", 0.0),
                .count = j.value<std::size_t>("kappa_ticks", 0),
                .last_trade_ns = last_trade_ns,
            });
            st.last_mid = last_mid;
            st.last_tick_ns = last_tick_ns;
            st.last_trade_ns = last_trade_ns;

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

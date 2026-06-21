#include "strategy/strategy/options_maker_strategy.h"

#include "strategy/refdata/exchange_id.h"
#include "strategy/strategy/reconciler.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OptionSide.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <pricer/pricing/black_scholes.h>
#include <unordered_set>
#include <vector>

namespace bpt::strategy::strategy {

std::string OptionsMakerStrategy::state_key(bpt::messages::ExchangeId::Value ex, const std::string& underlying) {
    return fmt::format("{}:{}", static_cast<int>(ex), underlying);
}

OptionsMakerStrategy::OptionsMakerStrategy(uint64_t correlation_id,
                                           const config::StrategyConfig& cfg,
                                           refdata::IRefdataClient& refdata,
                                           md::IMdClient* md,
                                           order::OrderManager* order_mgr,
                                           vol::IVolSurfaceClient* vol_client)
    : correlation_id_(correlation_id),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      risk_(cfg.risk),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr),
      vol_client_(vol_client) {
    const auto& p = cfg.params;

    front_n_expiries_ = static_cast<uint32_t>(p["front_n_expiries"].value<int64_t>().value_or(1));
    max_strikes_per_expiry_ = static_cast<uint32_t>(p["max_strikes_per_expiry"].value<int64_t>().value_or(8));
    risk_free_rate_ = p["risk_free_rate"].value<double>().value_or(0.0);
    quote_synthetic_strikes_ = p["quote_synthetic_strikes"].value<bool>().value_or(false);
    synthetic_size_mult_ = p["synthetic_size_mult"].value<double>().value_or(0.25);
    synthetic_max_strike_distance_pct_ = p["synthetic_max_strike_distance_pct"].value<double>().value_or(0.05);
    synthetic_smile_staleness_ns_ =
        static_cast<uint64_t>(p["synthetic_smile_staleness_ms"].value<double>().value_or(30000.0) * 1e6);
    vega_edge_vol_pts_ = p["vega_edge_vol_pts"].value<double>().value_or(0.005);
    per_quote_vega_budget_ = p["per_quote_vega_budget"].value<double>().value_or(20.0);
    max_book_vega_ = p["max_book_vega"].value<double>().value_or(500.0);
    max_book_delta_ = p["max_book_delta"].value<double>().value_or(1.0);
    max_book_gamma_ = p["max_book_gamma"].value<double>().value_or(50.0);
    requote_min_interval_ns_ =
        static_cast<uint64_t>(p["requote_min_interval_ms"].value<double>().value_or(500.0) * 1e6);

    enable_hedger_ = p["enable_hedger"].value<bool>().value_or(true);
    max_hedge_abs_delta_ = p["max_hedge_abs_delta"].value<double>().value_or(0.05);
    hedge_aggress_bps_ = p["hedge_aggress_bps"].value<double>().value_or(10.0);
    hedge_cooldown_ns_ = static_cast<uint64_t>(p["hedge_cooldown_ms"].value<double>().value_or(500.0) * 1e6);
    book_delta_sanity_ceiling_mult_ = p["book_delta_sanity_ceiling_mult"].value<double>().value_or(20.0);

    shutdown_flatten_positions_ = p["shutdown_flatten_positions"].value<bool>().value_or(true);
    shutdown_perp_cross_bps_ = p["shutdown_perp_cross_bps"].value<double>().value_or(20.0);

    if (auto* arr = p["underlyings"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                underlyings_.push_back(*v);
    }

    bpt::common::log::info(
        "[OptionsMaker] front_n_expiries={} max_strikes={} vega_edge={:.4f} per_quote_vega={:.1f} "
        "max_book_vega={:.1f} max_book_delta={:.2f} max_book_gamma={:.1f} requote_min={:.1f}ms",
        front_n_expiries_,
        max_strikes_per_expiry_,
        vega_edge_vol_pts_,
        per_quote_vega_budget_,
        max_book_vega_,
        max_book_delta_,
        max_book_gamma_,
        static_cast<double>(requote_min_interval_ns_) / 1e6);
    bpt::common::log::info(
        "[OptionsMaker] hedger enabled={} max_hedge_abs_delta={:.4f} aggress_bps={:.1f} cooldown={:.1f}ms",
        enable_hedger_,
        max_hedge_abs_delta_,
        hedge_aggress_bps_,
        static_cast<double>(hedge_cooldown_ns_) / 1e6);
}

void OptionsMakerStrategy::start() {
    bpt::common::log::info("[OptionsMaker] starting — universe resolved across {} underlyings ({} perp legs)",
                           underlyings_.size(),
                           states_.size());
    // Quote loop wires up in the next chunk. Today: log + idle.
}

void OptionsMakerStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    // Resolve the perp leg per underlying (delta hedger consumes our portfolio
    // snapshot to neutralise; strategy itself doesn't emit perp orders). Option
    // universe arrives dynamically via VolSurface and is resolved in
    // resolve_option_universe() once the first surface lands.
    const auto all = cache.get_all();
    for (const auto& inst : all) {
        if (inst.type != refdata::InstrumentType::PERPETUAL)
            continue;

        bool relevant = underlyings_.empty();
        for (const auto& u : underlyings_) {
            if (inst.base_currency == u) {
                relevant = true;
                break;
            }
        }
        if (!relevant)
            continue;

        const auto ex = refdata::to_exchange_id(inst.exchange);
        if (ex == bpt::messages::ExchangeId::NULL_VALUE)
            continue;

        const auto key = state_key(ex, inst.base_currency);
        auto& st = states_[key];
        st.underlying = inst.base_currency;
        st.exchange_id = ex;
        st.perp_instrument_id = inst.instrument_id;
        instrument_to_key_[inst.instrument_id] = key;

        bpt::common::log::info("[OptionsMaker] resolved perp leg {} id={} ({})",
                               inst.symbol,
                               inst.instrument_id,
                               inst.exchange);
    }

    resolve_option_universe(cache);
}

void OptionsMakerStrategy::on_delta(const refdata::Instrument& /*inst*/,
                                    bpt::messages::DeltaUpdateType::Value /*update_type*/) {
    // Option universe is rediscovered each VolSurface tick — refdata deltas
    // don't need to drive re-quote churn today. Revisit when listings update
    // mid-session change the strike band.
}

void OptionsMakerStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = instrument_to_key_.find(tick.instrumentId());
    if (it == instrument_to_key_.end())
        return;

    auto& st = states_[it->second];
    if (tick.instrumentId() != st.perp_instrument_id)
        return;

    st.perp_bid = tick.bidPrice();
    st.perp_ask = tick.askPrice();
}

void OptionsMakerStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {
    // Strategy doesn't react to trades on the underlying — quoting is surface-
    // driven, not flow-driven. Flow signals already live in radar and could
    // gate quoting in a later iteration.
}

void OptionsMakerStrategy::on_vol_surface(bpt::messages::VolSurface& surface) {
    const auto underlying = surface.getUnderlyingAsString();
    const auto key = state_key(surface.exchangeId(), underlying);

    auto state_it = states_.find(key);
    if (state_it == states_.end())
        return;
    auto& state = state_it->second;

    // Decode all surface points into OptionQuote rows. Theo is BS-priced
    // from the pricer's (now smile-fitted) implied vol — observed and
    // interpolated points alike carry the SVI-fit IV, so theo is consistent
    // across the universe and survives strikes with no venue maker. Venue
    // BBO is stored separately for the POST_ONLY cap in requote().
    auto& points = surface.points();
    while (points.hasNext()) {
        auto& pt = points.next();
        const uint64_t id = pt.instrumentId();
        auto& opt = state.options[id];
        opt.instrument_id = id;
        opt.expiry_date = pt.expiryDate();
        opt.strike = pt.strikePrice();
        opt.is_call = (pt.optionSide() == bpt::messages::OptionSide::CALL);
        opt.iv = pt.impliedVol();
        opt.delta = pt.delta();
        opt.gamma = pt.gamma();
        opt.vega = pt.vega();
        opt.theta = pt.theta();
        opt.time_to_expiry_y = pt.timeToExpiry();

        const double bp = pt.bidPrice();
        const double ap = pt.askPrice();
        const double fwd = pt.forwardPrice();
        const double T = pt.timeToExpiry();
        const double iv = pt.impliedVol();

        if (std::isfinite(bp) && std::isfinite(ap) && bp > 0.0 && ap > bp) {
            opt.venue_bid_price = bp;
            opt.venue_ask_price = ap;
        } else {
            // Clear so a strike that loses its venue maker between ticks
            // doesn't keep showing a stale BBO downstream.
            opt.venue_bid_price = 0.0;
            opt.venue_ask_price = 0.0;
        }

        if (T > 0.0 && iv > 0.0 && fwd > 0.0 && std::isfinite(iv)) {
            const auto bs = opt.is_call ? bpt::pricer::pricing::bs_call(fwd, opt.strike, T, risk_free_rate_, iv)
                                        : bpt::pricer::pricing::bs_put(fwd, opt.strike, T, risk_free_rate_, iv);
            // Deribit quotes option prices in the underlying (BTC/ETH);
            // bs_call/bs_put return USD prices. Convert back to native units
            // by dividing by the forward, matching the SBE pt.bidPrice()/
            // askPrice() convention so theo, bid, ask are unit-consistent.
            const bool inverse_quoted = (state.exchange_id == bpt::messages::ExchangeId::DERIBIT);
            opt.theo_price = inverse_quoted ? bs.price / fwd : bs.price;
        } else {
            opt.theo_price = 0.0;
        }

        // Forward is identical across all points at a given expiry; first
        // finite reading is enough to anchor the universe filter.
        if (state.forward_price == 0.0 && std::isfinite(fwd) && fwd > 0.0)
            state.forward_price = fwd;

        instrument_to_key_[id] = key;
    }

    // Phase 2 (synthetic quoting) bookkeeping — refresh once per surface tick.
    // last_surface_ns gates against quoting off a frozen smile if the pricer
    // goes quiet; observed_strikes_by_expiry is the lookup the wing-distance
    // safeguard uses to anchor synthetic quotes to nearby market data.
    state.last_surface_ns = bpt::common::util::TscClock::now_epoch_ns();
    state.observed_strikes_by_expiry.clear();
    for (const auto& [_, opt] : state.options) {
        if (opt.venue_bid_price > 0.0 && opt.venue_ask_price > opt.venue_bid_price)
            state.observed_strikes_by_expiry[opt.expiry_date].push_back(opt.strike);
    }
    for (auto& [_, strikes] : state.observed_strikes_by_expiry)
        std::sort(strikes.begin(), strikes.end());

    // Universe filter: front-N expiries × top-K strikes by closeness to fwd.
    std::vector<uint32_t> expiries;
    for (const auto& [_, opt] : state.options) {
        if (std::find(expiries.begin(), expiries.end(), opt.expiry_date) == expiries.end())
            expiries.push_back(opt.expiry_date);
    }
    std::sort(expiries.begin(), expiries.end());
    if (expiries.size() > front_n_expiries_)
        expiries.resize(front_n_expiries_);

    std::unordered_set<uint64_t> new_active;
    const double fwd = state.forward_price;
    for (uint32_t exp : expiries) {
        std::vector<std::pair<double, uint64_t>> by_dist;
        for (const auto& [id, opt] : state.options) {
            if (opt.expiry_date != exp)
                continue;
            const double d = (fwd > 0.0) ? std::abs(opt.strike - fwd) : 0.0;
            by_dist.emplace_back(d, id);
        }
        std::sort(by_dist.begin(), by_dist.end());
        const size_t take = std::min<size_t>(by_dist.size(), max_strikes_per_expiry_);
        for (size_t i = 0; i < take; ++i)
            new_active.insert(by_dist[i].second);
    }

    // Cancel quotes on options that just rolled out of the universe.
    for (uint64_t id : state.active_instruments) {
        if (new_active.find(id) != new_active.end())
            continue;
        auto it = state.options.find(id);
        if (it == state.options.end())
            continue;
        if (it->second.bid_order_id != 0)
            cancel_quote_side(it->second, /*bid_side=*/true);
        if (it->second.ask_order_id != 0)
            cancel_quote_side(it->second, /*bid_side=*/false);
    }
    state.active_instruments = std::move(new_active);

    recompute_greeks(state);

    const uint64_t now_ns = surface.timestampNs();
    int considered = 0, placed_bid = 0, placed_ask = 0;
    int skip_no_theo = 0, skip_no_vega = 0;
    int bid_breach_delta = 0, bid_breach_vega = 0, bid_breach_gamma = 0;
    int ask_breach_delta = 0, ask_breach_vega = 0, ask_breach_gamma = 0;
    for (uint64_t id : state.active_instruments) {
        auto it = state.options.find(id);
        if (it == state.options.end())
            continue;
        ++considered;
        const auto& q = it->second;
        if (q.theo_price <= 0.0) {
            ++skip_no_theo;
            continue;
        }
        if (!std::isfinite(q.vega) || q.vega <= 0.0) {
            ++skip_no_vega;
            continue;
        }
        // Project the same Greek-limit breach math requote uses so the
        // diagnostic surfaces which limit is suppressing each side.
        const double vega_floor = std::max(q.vega, 0.001);
        const double qty = per_quote_vega_budget_ / vega_floor;
        const auto over = [&](double cur, double per, double cap, bool bid_side) {
            const double pred = cur + (bid_side ? +qty * per : -qty * per);
            return std::abs(pred) > cap;
        };
        if (over(state.portfolio_delta, q.delta, max_book_delta_, true))
            ++bid_breach_delta;
        if (over(state.portfolio_vega, q.vega, max_book_vega_, true))
            ++bid_breach_vega;
        if (over(state.portfolio_gamma, q.gamma, max_book_gamma_, true))
            ++bid_breach_gamma;
        if (over(state.portfolio_delta, q.delta, max_book_delta_, false))
            ++ask_breach_delta;
        if (over(state.portfolio_vega, q.vega, max_book_vega_, false))
            ++ask_breach_vega;
        if (over(state.portfolio_gamma, q.gamma, max_book_gamma_, false))
            ++ask_breach_gamma;

        const uint64_t pre_bid = q.bid_order_id;
        const uint64_t pre_ask = q.ask_order_id;
        requote(state, it->second, now_ns);
        if (it->second.bid_order_id != 0 && pre_bid == 0)
            ++placed_bid;
        if (it->second.ask_order_id != 0 && pre_ask == 0)
            ++placed_ask;
    }
    bpt::common::log::info(
        "[OptionsMaker] surface {} fwd={:.2f} pts={} active={} considered={} "
        "placed bid={}/ask={} skip theo={} vega={} "
        "bid_breach(Δ={} V={} Γ={}) ask_breach(Δ={} V={} Γ={})",
        state.underlying,
        state.forward_price,
        state.options.size(),
        state.active_instruments.size(),
        considered,
        placed_bid,
        placed_ask,
        skip_no_theo,
        skip_no_vega,
        bid_breach_delta,
        bid_breach_vega,
        bid_breach_gamma,
        ask_breach_delta,
        ask_breach_vega,
        ask_breach_gamma);

    // Spot moves between surface ticks change option deltas → re-evaluate
    // the hedge here in addition to after every option fill.
    if (enable_hedger_)
        maybe_hedge(state, now_ns);
}

void OptionsMakerStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    using bpt::messages::ExecStatus;

    const uint64_t oid = rpt.orderId();

    // Route by order_id first (cheapest, matches orders this instance placed).
    // Fall back to instrument_id when the order_id isn't ours — handles
    // orphan fills from a previous strategy instance whose orders survived
    // WSL suspend / crash / manual restart and got hit before the new
    // instance learned about them. Without this fallback book delta would
    // accumulate silently without ever triggering the hedger (2026-05-15
    // pm: positions=3 observed in account snapshot, zero corresponding
    // [OptionsMaker] fill logs).
    std::string key;
    auto key_it = order_to_key_.find(oid);
    if (key_it != order_to_key_.end()) {
        key = key_it->second;
    } else {
        auto inst_it = instrument_to_key_.find(rpt.instrumentId());
        if (inst_it == instrument_to_key_.end())
            return;
        key = inst_it->second;
        bpt::common::log::warn(
            "[OptionsMaker] exec report for unknown order_id={} but instrument {} matches — "
            "applying as orphan fill",
            oid,
            rpt.instrumentId());
    }

    auto state_it = states_.find(key);
    if (state_it == states_.end())
        return;
    auto& state = state_it->second;

    const bool is_perp = order_is_perp_hedge_.count(oid) && order_is_perp_hedge_[oid];
    const auto status = rpt.status();

    // ExecutionReport prices and quantities are int64 scaled by 1e8 — same
    // convention ShortVol decodes against.
    const double fill_qty = static_cast<double>(rpt.filledQty()) / 1e8;
    const double fill_price = static_cast<double>(rpt.price()) / 1e8;
    const bool is_buy = (rpt.side() == bpt::messages::OrderSide::BUY);
    const double signed_qty = is_buy ? fill_qty : -fill_qty;

    if (is_perp) {
        if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
            // Reverse of the maybe_hedge unit conversion. Deribit's perp exec
            // report's filledQty is in USD-equivalent contract units (matching
            // what we sent). Convert back to BTC so perp_position_qty stays
            // in the same units as portfolio_delta — otherwise book_delta =
            // BTC_options + USD_perp produces nonsense and triggers a
            // hedge feedback loop (2026-05-15 cascade post-mortem).
            const double fill_qty_native = fill_price > 0.0 ? fill_qty / fill_price : 0.0;
            const double signed_qty_native = is_buy ? fill_qty_native : -fill_qty_native;
            state.perp_position_qty += signed_qty_native;
            bpt::common::log::info(
                "[OptionsMaker] hedge fill {} {} qty_usd={:.2f} qty_native={:.6f} price={:.2f} "
                "perp_pos={:.6f} book_delta={:.6f}",
                state.underlying,
                is_buy ? "BUY" : "SELL",
                fill_qty,
                fill_qty_native,
                fill_price,
                state.perp_position_qty,
                state.portfolio_delta + state.perp_position_qty);
        }
        if (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED) {
            state.perp_order_id = 0;
            order_to_key_.erase(oid);
            order_is_perp_hedge_.erase(oid);
            live_orders_.erase(oid);
        }
        return;
    }

    // Option-side exec report — route to OptionQuote, apply fill, requote that
    // side. Hedge gets re-evaluated after the Greek refresh.
    auto opt_it = state.options.find(rpt.instrumentId());
    if (opt_it == state.options.end())
        return;
    auto& opt = opt_it->second;

    const bool was_bid_side = order_is_bid_side_.count(oid) ? order_is_bid_side_[oid] : false;

    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        opt.position_qty += signed_qty;
        opt.entry_price = fill_price;

        bpt::common::log::info("[OptionsMaker] fill {} {} K={:.0f} qty={:.6f} price={:.4f} pos={:.6f}",
                               state.underlying,
                               is_buy ? "BUY" : "SELL",
                               opt.strike,
                               fill_qty,
                               fill_price,
                               opt.position_qty);

        recompute_greeks(state);
        if (enable_hedger_)
            maybe_hedge(state, rpt.timestampNs());
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED) {
        if (was_bid_side)
            opt.bid_order_id = 0;
        else
            opt.ask_order_id = 0;
        order_to_key_.erase(oid);
        order_is_bid_side_.erase(oid);
        live_orders_.erase(oid);
    }
}

std::size_t OptionsMakerStrategy::on_account_snapshot(bpt::messages::AccountSnapshot& snap) {
    // Startup reconciliation: any exchange positions we don't know about
    // (orphan fills from a previous strategy instance, manual UI orders,
    // host-sleep-related cancel-on-disconnect gaps) get seeded into
    // state.options + state.perp_position_qty so the hedger fires on the
    // next surface tick and book delta gets neutralised.
    //
    // Without this, an unbounded amount of directional risk can accumulate
    // silently across restarts (2026-05-15 pm: positions=4 unhedged after
    // WSL suspend → strategy restart cycle).

    // strategy_service_wiring's log line calls snap.positions().count()
    // before handing us the message, which advances the SBE group cursor.
    // Without rewind, extract_exchange_position_rows below reads garbage.
    // (Same lesson as AS — see comment on its on_account_snapshot.)
    snap.sbeRewind();

    const auto rows = extract_exchange_position_rows(snap);
    if (rows.empty())
        return 0;

    // Build a symbol → Instrument lookup from refdata. Done once per snapshot
    // (rare event) rather than maintaining a sticky map — keeps the cache as
    // the single source of truth.
    std::unordered_map<std::string, refdata::Instrument> by_symbol;
    by_symbol.reserve(refdata_.cache().size());
    for (const auto& inst : refdata_.cache().get_all())
        by_symbol.emplace(inst.symbol, inst);

    std::unordered_set<std::string> touched_state_keys;
    std::size_t seeded_option_legs = 0;
    std::size_t seeded_perp_legs = 0;

    for (const auto& [symbol, row] : rows) {
        if (row.net_qty_e8 == 0)
            continue;

        auto it = by_symbol.find(symbol);
        if (it == by_symbol.end()) {
            bpt::common::log::warn("[OptionsMaker] account position {} qty={} not in refdata — skipping",
                                   symbol,
                                   static_cast<double>(row.net_qty_e8) / 1e8);
            continue;
        }
        const auto& inst = it->second;
        const double qty = static_cast<double>(row.net_qty_e8) / 1e8;

        // Match by base currency (matches our state-keying convention).
        // For Deribit, ETHEREUM perp = ETH base, BTC perp = BTC base.
        const auto state_key_str = state_key(snap.exchangeId(), inst.base_currency);
        auto sit = states_.find(state_key_str);
        if (sit == states_.end()) {
            bpt::common::log::warn(
                "[OptionsMaker] account position {} (base={}) — no state tracking this underlying, skipping",
                symbol,
                inst.base_currency);
            continue;
        }
        auto& st = sit->second;

        if (inst.type == refdata::InstrumentType::PERPETUAL) {
            if (inst.instrument_id == st.perp_instrument_id) {
                // Same unit reconciliation as on_exec_report: Deribit reports
                // perp size in USD-equivalent (matching its `amount` API
                // semantics). Convert to BTC so perp_position_qty stays in
                // the units portfolio_delta uses. avg_entry_price is the
                // best price reference available here.
                const double qty_native = row.avg_entry_price > 0.0 ? qty / row.avg_entry_price : qty;
                st.perp_position_qty = qty_native;
                ++seeded_perp_legs;
                touched_state_keys.insert(state_key_str);
                bpt::common::log::info(
                    "[OptionsMaker] reconciled perp {} qty_usd={:.2f} qty_native={:.6f} entry={:.2f}",
                    symbol,
                    qty,
                    qty_native,
                    row.avg_entry_price);
            }
            continue;
        }

        if (inst.type != refdata::InstrumentType::OPTION)
            continue;

        // Upsert the option into state.options. Most likely it's already
        // there (we're quoting that strike). If not, add a minimal entry
        // so Greeks math doesn't NPE; the next vol surface tick will
        // populate IV/Δ/Γ/Vega/Θ.
        auto& opt = st.options[inst.instrument_id];
        opt.instrument_id = inst.instrument_id;
        opt.strike = inst.strike_price;
        opt.expiry_date = inst.expiry_date;
        opt.is_call = (inst.option_side == refdata::OptionSide::CALL);
        opt.position_qty = qty;
        opt.entry_price = row.avg_entry_price;
        instrument_to_key_[inst.instrument_id] = state_key_str;
        ++seeded_option_legs;
        touched_state_keys.insert(state_key_str);
        bpt::common::log::info("[OptionsMaker] reconciled option {} K={:.0f} qty={:.6f} entry={:.4f}",
                               symbol,
                               inst.strike_price,
                               qty,
                               row.avg_entry_price);
    }

    // Refresh book Greeks + fire hedger for every state we just touched.
    // Hedger may be a no-op if we don't yet have Greeks for the reconciled
    // options (vol surface hasn't arrived) — that's fine, the next surface
    // tick's maybe_hedge call will see real numbers and neutralise.
    const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
    for (const auto& key : touched_state_keys) {
        auto& st = states_.at(key);
        recompute_greeks(st);
        if (enable_hedger_)
            maybe_hedge(st, now_ns);
    }

    bpt::common::log::info(
        "[OptionsMaker] account reconciliation: {} option legs + {} perp legs seeded across {} states",
        seeded_option_legs,
        seeded_perp_legs,
        touched_state_keys.size());

    return seeded_option_legs + seeded_perp_legs;
}

PortfolioState OptionsMakerStrategy::get_portfolio_state() {
    // Aggregate legs across all underlyings so the console can render the
    // Greeks panel as soon as we hold any positions. Quote-only legs (no
    // position) are skipped to keep the wire small. Surface points include
    // EVERY known strike (regardless of position) so the smile + heatmap
    // panels have a continuous IV curve to render — each point carries
    // its `underlying` so the console can split BTC and ETH cleanly.
    PortfolioState ps;
    for (const auto& [_, st] : states_) {
        for (const auto& [__, opt] : st.options) {
            // Legs: only positions we hold (Greeks panel is position-focused).
            if (opt.position_qty != 0.0) {
                PortfolioState::Leg leg;
                leg.instrument_id = opt.instrument_id;
                leg.underlying = st.underlying;
                leg.expiry_date = opt.expiry_date;
                leg.strike = opt.strike;
                leg.is_call = opt.is_call;
                leg.is_option = true;
                leg.qty = opt.position_qty;
                leg.entry_price = opt.entry_price;
                leg.mark_price = opt.theo_price;
                leg.iv = opt.iv;
                leg.delta = opt.delta;
                leg.gamma = opt.gamma;
                leg.vega = opt.vega;
                leg.theta = opt.theta;
                ps.legs.push_back(leg);
            }

            // Surface points: every option we have smile-fitted IV + Greeks
            // for, including interpolated synthetic strikes — those give
            // the smile curve continuity in regions where no venue maker
            // exists. Skip points where the IV failed to converge.
            if (std::isfinite(opt.iv) && opt.iv > 0.0 && opt.time_to_expiry_y > 0.0) {
                PortfolioState::SurfacePoint sp;
                sp.instrument_id = opt.instrument_id;
                sp.underlying = st.underlying;
                sp.expiry_date = opt.expiry_date;
                sp.strike = opt.strike;
                sp.is_call = opt.is_call;
                sp.iv = opt.iv;
                // Pre-Phase-1, observed strikes carried per-side IV from
                // venue bid/ask; pricer now re-projects everything onto
                // the SVI fit so bid/ask IV are only set on observed
                // points (we have venue prices) — leave at 0 for
                // interpolated. Strategy doesn't track these per-point
                // separately today; emit 0 as the "use mid IV" sentinel.
                sp.bid_iv = 0.0;
                sp.ask_iv = 0.0;
                sp.delta = opt.delta;
                sp.time_to_expiry = opt.time_to_expiry_y;
                ps.surface_points.push_back(sp);
            }
        }
        // Surface the perp hedge leg when we hold any so the console
        // shows the actual neutral book, not just the option-side Greeks.
        if (st.perp_position_qty != 0.0 && st.perp_instrument_id != 0) {
            PortfolioState::Leg leg;
            leg.instrument_id = st.perp_instrument_id;
            leg.underlying = st.underlying;
            leg.is_option = false;
            leg.qty = st.perp_position_qty;
            leg.mark_price = 0.5 * (st.perp_bid + st.perp_ask);
            leg.delta = 1.0;  // linear perp
            ps.legs.push_back(leg);
        }
        ps.portfolio_delta += st.portfolio_delta + st.perp_position_qty;
        ps.portfolio_gamma += st.portfolio_gamma;
        ps.portfolio_vega += st.portfolio_vega;
        ps.portfolio_theta += st.portfolio_theta;
    }
    return ps;
}

std::string OptionsMakerStrategy::get_strategy_state_json() {
    // Compact handwritten JSON — strategy_lib doesn't depend on nlohmann/json,
    // and the payload shape is small enough that pulling it in for this would
    // be a poor trade. Sanitize every double through `j` because fmt prints
    // NaN/Inf as the literal strings "nan"/"inf", which are not valid JSON
    // — bites on expired options whose time_to_expiry → 0 makes BS Greeks
    // diverge.
    auto j = [](double v) {
        return std::isfinite(v) ? v : 0.0;
    };
    std::string out =
        fmt::format(R"({{"type":"strategyState","kind":"OptionsMaker","risk_halted":{},"underlyings":[)", risk_halted_);
    bool first = true;
    for (const auto& [_, st] : states_) {
        if (!first)
            out += ',';
        first = false;
        out += fmt::format(R"({{"underlying":"{}","exchange":"{}","option_count":{},"portfolio_delta":{:.4f},)"
                           R"("portfolio_vega":{:.2f},"portfolio_gamma":{:.4f},"portfolio_theta":{:.2f},)"
                           R"("perp_position":{:.6f},"book_delta":{:.4f},"hedge_in_flight":{},)"
                           R"("active_strikes":[)",
                           st.underlying,
                           bpt::messages::ExchangeId::c_str(st.exchange_id),
                           st.options.size(),
                           j(st.portfolio_delta),
                           j(st.portfolio_vega),
                           j(st.portfolio_gamma),
                           j(st.portfolio_theta),
                           j(st.perp_position_qty),
                           j(st.portfolio_delta + st.perp_position_qty),
                           st.perp_order_id != 0);

        // Strike-level view — include any option in the active universe OR
        // anything we still hold inventory on. Sorted by distance to forward
        // and capped at kMaxStrikesEmit so the console renders something
        // readable rather than a 50-row scroll. Bridge uses FragmentAssembler
        // so this is a display sanity cap, not an MTU constraint.
        constexpr std::size_t kMaxStrikesEmit = 12;
        std::vector<std::pair<double, const OptionQuote*>> candidates;
        candidates.reserve(st.options.size());
        for (const auto& [iid, opt] : st.options) {
            const bool in_universe = st.active_instruments.count(iid) > 0;
            const bool has_position = opt.position_qty != 0.0;
            if (!in_universe && !has_position)
                continue;
            // Held-inventory strikes get distance 0 so they rank ahead of
            // pure quote-only strikes — operator always wants to see what
            // they're holding even at deep wings.
            const double dist = has_position ? 0.0 : std::abs(opt.strike - st.forward_price);
            candidates.emplace_back(dist, &opt);
        }
        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (candidates.size() > kMaxStrikesEmit)
            candidates.resize(kMaxStrikesEmit);

        bool s_first = true;
        for (const auto& [_dist, opt_ptr] : candidates) {
            const auto& opt = *opt_ptr;
            const double venue_mid = (opt.venue_bid_price > 0.0 && opt.venue_ask_price > opt.venue_bid_price)
                                         ? 0.5 * (opt.venue_bid_price + opt.venue_ask_price)
                                         : 0.0;
            if (!s_first)
                out += ',';
            s_first = false;
            out += fmt::format(R"({{"strike":{:.0f},"expiry":{},"is_call":{},"theo":{:.6f},"venue_mid":{:.6f},)"
                               R"("our_bid":{:.6f},"our_ask":{:.6f},"position":{:.4f},"delta":{:.4f},"vega":{:.4f}}})",
                               j(opt.strike),
                               opt.expiry_date,
                               opt.is_call,
                               j(opt.theo_price),
                               j(venue_mid),
                               opt.bid_order_id != 0 ? j(opt.bid_price) : 0.0,
                               opt.ask_order_id != 0 ? j(opt.ask_price) : 0.0,
                               j(opt.position_qty),
                               j(opt.delta),
                               j(opt.vega));
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

void OptionsMakerStrategy::on_shutdown_flatten() {
    // Two-phase shutdown:
    //   1) Cancel every order we've seen go out on the wire (sourced from
    //      live_orders_ since per-option fields can be transiently cleared
    //      mid-cancel-and-replace; see 2026-05-15 post-mortem).
    //   2) If `shutdown_flatten_positions_` is set, IOC-close every non-zero
    //      option and perp position. Options cross to the touch (illiquid
    //      wings won't fill on a small mid-cross); perp crosses mid by
    //      `shutdown_perp_cross_bps_`. AS-style. Operator can disable via
    //      `shutdown_flatten_positions=false` in [strategy.params] if running
    //      on a venue where crossing wings would be too expensive.
    if (!order_mgr_) {
        bpt::common::log::info("[OptionsMaker] shutdown_flatten — no order_mgr, nothing to do");
        return;
    }

    const auto snapshot = live_orders_;  // copy; cancel_order may trigger exec reports that mutate the map
    for (const auto& [oid, ref] : snapshot)
        order_mgr_->send_cancel(order::CancelOrderRequest{oid, ref.exchange_id, ref.instrument_id});

    bpt::common::log::info("[OptionsMaker] shutdown_flatten cancelled {} live orders (option quotes + perp hedges)",
                           snapshot.size());

    // Clear the per-option order_id fields too so the console doesn't
    // continue advertising the cancelled orders as live until exec reports
    // land in the drain window.
    for (auto& [_, st] : states_) {
        for (auto& [__, opt] : st.options) {
            opt.bid_order_id = 0;
            opt.ask_order_id = 0;
        }
        st.perp_order_id = 0;
    }

    if (!shutdown_flatten_positions_) {
        bpt::common::log::info("[OptionsMaker] shutdown_flatten_positions=false — leaving net positions on the book");
        return;
    }

    int opt_unwinds = 0;
    int opt_skipped = 0;
    int perp_unwinds = 0;
    int perp_skipped = 0;

    for (auto& [_, st] : states_) {
        // Options — cross to the visible touch. Anything less won't fill on
        // illiquid testnet wings. Skip if we have no BBO (no surface tick
        // for this strike yet — operator handles manually).
        for (auto& [__, opt] : st.options) {
            if (opt.position_qty == 0.0)
                continue;
            if (opt.venue_bid_price <= 0.0 || opt.venue_ask_price <= opt.venue_bid_price) {
                bpt::common::log::warn(
                    "[OptionsMaker] shutdown_flatten: option iid={} K={:.0f} qty={:.4f} has no BBO, skipping",
                    opt.instrument_id,
                    opt.strike,
                    opt.position_qty);
                ++opt_skipped;
                continue;
            }
            const bool sell = opt.position_qty > 0.0;
            const double price = sell ? opt.venue_bid_price : opt.venue_ask_price;
            const double qty = std::abs(opt.position_qty);
            const uint64_t oid = order_mgr_
                                     ->send_new_order(order::NewOrderRequest{
                                         .instrument_id = opt.instrument_id,
                                         .exchange_id = st.exchange_id,
                                         .side = sell ? bpt::messages::OrderSide::SELL : bpt::messages::OrderSide::BUY,
                                         .type = bpt::messages::OrderType::LIMIT,
                                         .tif = bpt::messages::TimeInForce::IOC,
                                         .price = price,
                                         .qty = qty,
                                     })
                                     .order_id();
            if (oid == 0) {
                ++opt_skipped;
                continue;
            }
            live_orders_[oid] = OrderRef{st.exchange_id, opt.instrument_id};
            order_to_key_[oid] = state_key(st.exchange_id, st.underlying);
            ++opt_unwinds;
            bpt::common::log::info(
                "[OptionsMaker] shutdown_flatten: option iid={} K={:.0f} {} qty={:.4f} @ {:.4f} → oid={}",
                opt.instrument_id,
                opt.strike,
                sell ? "SELL" : "BUY",
                qty,
                price,
                oid);
        }

        // Perp — mid +/- shutdown_perp_cross_bps. Tight spread on Deribit
        // BTC/ETH perp makes this safe. Same BTC→USD-notional conversion as
        // maybe_hedge (Deribit `amount` is USD value for inverse perps).
        if (st.perp_position_qty != 0.0 && st.perp_instrument_id != 0) {
            if (st.perp_bid <= 0.0 || st.perp_ask <= st.perp_bid) {
                bpt::common::log::warn("[OptionsMaker] shutdown_flatten: perp iid={} qty={:.6f} has no BBO, skipping",
                                       st.perp_instrument_id,
                                       st.perp_position_qty);
                ++perp_skipped;
            } else {
                const bool sell = st.perp_position_qty > 0.0;
                const double mid = 0.5 * (st.perp_bid + st.perp_ask);
                const double cross = shutdown_perp_cross_bps_ * 1e-4;
                const double price = sell ? mid * (1.0 - cross) : mid * (1.0 + cross);
                const double qty_native = std::abs(st.perp_position_qty);
                const double qty_usd = qty_native * price;
                const uint64_t oid =
                    order_mgr_
                        ->send_new_order(order::NewOrderRequest{
                            .instrument_id = st.perp_instrument_id,
                            .exchange_id = st.exchange_id,
                            .side = sell ? bpt::messages::OrderSide::SELL : bpt::messages::OrderSide::BUY,
                            .type = bpt::messages::OrderType::LIMIT,
                            .tif = bpt::messages::TimeInForce::IOC,
                            .price = price,
                            .qty = qty_usd,
                        })
                        .order_id();
                if (oid == 0) {
                    ++perp_skipped;
                } else {
                    live_orders_[oid] = OrderRef{st.exchange_id, st.perp_instrument_id};
                    order_to_key_[oid] = state_key(st.exchange_id, st.underlying);
                    order_is_perp_hedge_[oid] = true;
                    st.perp_order_id = oid;
                    ++perp_unwinds;
                    bpt::common::log::info(
                        "[OptionsMaker] shutdown_flatten: perp {} qty_native={:.6f} qty_usd={:.2f} @ {:.2f} → oid={}",
                        sell ? "SELL" : "BUY",
                        qty_native,
                        qty_usd,
                        price,
                        oid);
                }
            }
        }
    }

    bpt::common::log::info(
        "[OptionsMaker] shutdown_flatten: fired {} option IOC + {} perp IOC, skipped {} option + {} perp",
        opt_unwinds,
        perp_unwinds,
        opt_skipped,
        perp_skipped);
}

bool OptionsMakerStrategy::has_pending_flatten() const {
    // Any tracked live order on the wire counts. Positions don't gate
    // shutdown anymore (we don't flatten them on shutdown intentionally) —
    // but the drain loop above us in StrategyService still needs to wait
    // for cancel acks to land.
    if (!live_orders_.empty())
        return true;
    for (const auto& [_, st] : states_)
        if (st.perp_position_qty != 0.0)
            return true;
    return false;
}

void OptionsMakerStrategy::resolve_option_universe(const refdata::InstrumentCache& /*cache*/) {
    // Option universe is filtered to (front N expiries × ATM strike band) when
    // the first VolSurface arrives; that's where we have the forward price
    // needed for the band. Stubbed today.
}

void OptionsMakerStrategy::recompute_greeks(UnderlyingState& state) {
    double d = 0.0, g = 0.0, v = 0.0, t = 0.0;
    for (const auto& [_, opt] : state.options) {
        d += opt.position_qty * opt.delta;
        g += opt.position_qty * opt.gamma;
        v += opt.position_qty * opt.vega;
        t += opt.position_qty * opt.theta;
    }
    state.portfolio_delta = d;
    state.portfolio_gamma = g;
    state.portfolio_vega = v;
    state.portfolio_theta = t;
}

void OptionsMakerStrategy::requote(UnderlyingState& state, OptionQuote& opt, uint64_t now_ns) {
    if (!order_mgr_)
        return;
    if (risk_halted_)
        return;

    // Per-option cadence throttle — surface ticks at high frequency on a busy
    // venue and we don't want to cancel/replace on every one.
    if (opt.bid_order_id != 0 || opt.ask_order_id != 0) {
        if (now_ns - opt.last_quote_ns < requote_min_interval_ns_)
            return;
    }

    if (opt.theo_price <= 0.0 || !std::isfinite(opt.vega) || opt.vega <= 0.0)
        return;

    // Phase 2 — synthetic-strike gates. A "synthetic" strike is one with no
    // observed venue BBO this tick; theo comes solely from the SVI fit, so
    // POST_ONLY cap downstream has nothing to clamp against and we'd be
    // the sole maker. Three layered safeguards:
    //   (1) master switch `quote_synthetic_strikes_` (default off)
    //   (2) smile staleness — refuse to quote off a frozen surface
    //   (3) wing-distance cap — nearest observed strike in this expiry
    //       must sit within `synthetic_max_strike_distance_pct_` × forward,
    //       so we never quote into a complete vacuum
    // Size on synthetic strikes is further scaled by `synthetic_size_mult_`
    // below in the qty calc.
    const bool is_synthetic = (opt.venue_bid_price <= 0.0 || opt.venue_ask_price <= opt.venue_bid_price);
    if (is_synthetic) {
        if (!quote_synthetic_strikes_)
            return;
        if (state.last_surface_ns == 0 || now_ns - state.last_surface_ns > synthetic_smile_staleness_ns_)
            return;
        if (state.forward_price <= 0.0)
            return;
        auto it = state.observed_strikes_by_expiry.find(opt.expiry_date);
        if (it == state.observed_strikes_by_expiry.end() || it->second.empty())
            return;
        const auto& strikes = it->second;
        auto lb = std::lower_bound(strikes.begin(), strikes.end(), opt.strike);
        double min_dist = std::numeric_limits<double>::infinity();
        if (lb != strikes.end())
            min_dist = std::min(min_dist, std::abs(*lb - opt.strike));
        if (lb != strikes.begin())
            min_dist = std::min(min_dist, std::abs(*(lb - 1) - opt.strike));
        if (min_dist > synthetic_max_strike_distance_pct_ * state.forward_price)
            return;
    }

    // Half-spread expressed in price units: a vega-edge of e.g. 50 IV bps
    // translates to vega × 0.005 of premium charged on each side.
    // Unit reconciliation: the pricer reports vega in USD-equivalent ("dollar
    // vega"), but Deribit option prices are quoted in BTC. Convert to native
    // price units by dividing by the underlying forward (USD/BTC). After this
    // half_spread is in BTC, comparable to theo.
    const double vega_in_price_units = state.forward_price > 0.0 ? opt.vega / state.forward_price : opt.vega;
    const double half_spread = vega_in_price_units * vega_edge_vol_pts_;
    double target_bid = opt.theo_price - half_spread;
    double target_ask = opt.theo_price + half_spread;

    // Theo-vs-venue-mid divergence diagnostic. When |smile theo - venue mid|
    // exceeds half_spread, one of our quotes lies outside the venue's BBO
    // — that's where edge gets captured (or, if persistent and the smile
    // fit is wrong, where we'd be the wrong-way taker). Log so the
    // operator can eyeball the smile vs market drift in real time.
    if (opt.venue_bid_price > 0.0 && opt.venue_ask_price > opt.venue_bid_price && half_spread > 0.0) {
        const double venue_mid = 0.5 * (opt.venue_bid_price + opt.venue_ask_price);
        const double dislocation = opt.theo_price - venue_mid;
        if (std::abs(dislocation) > half_spread) {
            bpt::common::log::info(
                "[OptionsMaker] dislocation iid={} K={:.0f} {} venue_mid={:.5f} theo={:.5f} "
                "diff={:+.5f} half_spread={:.5f} ratio={:+.2f}x",
                opt.instrument_id,
                opt.strike,
                opt.is_call ? "C" : "P",
                venue_mid,
                opt.theo_price,
                dislocation,
                half_spread,
                dislocation / half_spread);
        }
    }

    // POST_ONLY safety: never quote at or through the venue's touch — that's
    // an exchange-side reject and a free price-discovery leak. 5 bp pad
    // (was 1 bp until 2026-05-15) absorbs touch movement between our read
    // and the exchange ack; 1 bp let 13 `order_overlap` rejects through on
    // the first live session.
    constexpr double kCrossPadBps = 0.0005;  // 5 bp
    const double ask_cap = opt.venue_ask_price * (1.0 - kCrossPadBps);
    const double bid_floor = opt.venue_bid_price * (1.0 + kCrossPadBps);
    if (opt.venue_ask_price > 0.0)
        target_bid = std::min(target_bid, ask_cap);
    if (opt.venue_bid_price > 0.0)
        target_ask = std::max(target_ask, bid_floor);

    if (target_bid <= 0.0 || target_ask <= target_bid)
        return;

    // Debounce — skip the cancel+replace round-trip if both sides have moved
    // less than half our edge from the current quote. Without this, every
    // surface tick churns 32 cancels + 32 places through the OGW even when
    // the touch is unchanged, saturating the OGW's open-orders counter
    // because Deribit's cancel-ack latency lags the place rate (2026-05-15
    // post-mortem: `open_orders=100 >= max_open=100` rejected ~1100 orders
    // in 60s).
    if (opt.bid_order_id != 0 && opt.ask_order_id != 0) {
        const double tolerance = 0.5 * half_spread;
        if (std::abs(target_bid - opt.bid_price) < tolerance && std::abs(target_ask - opt.ask_price) < tolerance) {
            opt.last_quote_ns = now_ns;  // count this as a successful tick — quote still good
            return;
        }
    }

    // Vega-budget sizing — fewer contracts on high-vega ATMs, more on far OTMs.
    // Floor vega with a tiny epsilon (0.001) only to avoid divide-by-zero on
    // expiring wings. Note Deribit options report vega in BTC-equivalent units
    // (~0.05 ATM); per_quote_vega_budget must be tuned to that scale, not the
    // USD-vega scale a traditional equity option desk would use.
    const double vega_floor = std::max(opt.vega, 0.001);
    // Synthetic strikes have no venue-BBO sanity check; scale size down to
    // reflect lower confidence in our sole-maker theo.
    const double size_mult = is_synthetic ? synthetic_size_mult_ : 1.0;
    const double qty = (per_quote_vega_budget_ / vega_floor) * size_mult;
    if (qty <= 0.0)
        return;

    // Pre-trade Greek-limit gate — would a fill at our quote push the book
    // past any hard cap? Each side gated independently so one direction can
    // still quote when the other is suppressed.
    const auto exceeds = [&](double current, double per_unit, double q, double cap, bool is_bid) {
        const double predicted = current + (is_bid ? +q * per_unit : -q * per_unit);
        return std::abs(predicted) > cap;
    };
    const bool bid_breaches = exceeds(state.portfolio_delta, opt.delta, qty, max_book_delta_, true) ||
                              exceeds(state.portfolio_vega, opt.vega, qty, max_book_vega_, true) ||
                              exceeds(state.portfolio_gamma, opt.gamma, qty, max_book_gamma_, true);
    const bool ask_breaches = exceeds(state.portfolio_delta, opt.delta, qty, max_book_delta_, false) ||
                              exceeds(state.portfolio_vega, opt.vega, qty, max_book_vega_, false) ||
                              exceeds(state.portfolio_gamma, opt.gamma, qty, max_book_gamma_, false);

    // Cancel-and-replace each side. We don't try to detect "price unchanged"
    // and skip — OrderManager dedupes upstream, and quote freshness is more
    // important than message economy at this throttle rate.
    if (opt.bid_order_id != 0)
        cancel_quote_side(opt, /*bid_side=*/true);
    if (opt.ask_order_id != 0)
        cancel_quote_side(opt, /*bid_side=*/false);

    if (!bid_breaches) {
        const uint64_t oid = order_mgr_
                                 ->send_new_order(order::NewOrderRequest{
                                     .instrument_id = opt.instrument_id,
                                     .exchange_id = state.exchange_id,
                                     .side = bpt::messages::OrderSide::BUY,
                                     .type = bpt::messages::OrderType::LIMIT,
                                     .tif = bpt::messages::TimeInForce::GTC,
                                     .price = target_bid,
                                     .qty = qty,
                                     .exec_inst = {.post_only = true},
                                 })
                                 .order_id();
        if (oid != 0) {
            opt.bid_order_id = oid;
            opt.bid_price = target_bid;
            opt.bid_qty = qty;
            order_to_key_[oid] = state_key(state.exchange_id, state.underlying);
            order_is_bid_side_[oid] = true;
            live_orders_[oid] = OrderRef{state.exchange_id, opt.instrument_id};
        }
    }

    if (!ask_breaches) {
        const uint64_t oid = order_mgr_
                                 ->send_new_order(order::NewOrderRequest{
                                     .instrument_id = opt.instrument_id,
                                     .exchange_id = state.exchange_id,
                                     .side = bpt::messages::OrderSide::SELL,
                                     .type = bpt::messages::OrderType::LIMIT,
                                     .tif = bpt::messages::TimeInForce::GTC,
                                     .price = target_ask,
                                     .qty = qty,
                                     .exec_inst = {.post_only = true},
                                 })
                                 .order_id();
        if (oid != 0) {
            opt.ask_order_id = oid;
            opt.ask_price = target_ask;
            opt.ask_qty = qty;
            order_to_key_[oid] = state_key(state.exchange_id, state.underlying);
            order_is_bid_side_[oid] = false;
            live_orders_[oid] = OrderRef{state.exchange_id, opt.instrument_id};
        }
    }

    opt.last_quote_ns = now_ns;
}

void OptionsMakerStrategy::cancel_quote_side(OptionQuote& opt, bool bid_side) {
    if (!order_mgr_)
        return;
    uint64_t& oid_ref = bid_side ? opt.bid_order_id : opt.ask_order_id;
    if (oid_ref == 0)
        return;

    // Look up the venue from the stored routing map — same way ShortVol
    // unwinds order_to_key_ on terminal exec reports.
    auto key_it = order_to_key_.find(oid_ref);
    if (key_it != order_to_key_.end()) {
        auto state_it = states_.find(key_it->second);
        if (state_it != states_.end())
            order_mgr_->send_cancel(
                order::CancelOrderRequest{oid_ref, state_it->second.exchange_id, opt.instrument_id});
    }
    oid_ref = 0;
}

void OptionsMakerStrategy::maybe_hedge(UnderlyingState& state, uint64_t now_ns) {
    if (!order_mgr_)
        return;
    if (risk_halted_)
        return;
    if (state.perp_instrument_id == 0)
        return;
    // In-flight hedge — let it land before stacking another.
    if (state.perp_order_id != 0)
        return;
    if (now_ns - state.last_hedge_ns < hedge_cooldown_ns_)
        return;

    // True book delta = option-side delta + perp position. Hedge the residual
    // so book_delta_after = 0.
    const double book_delta = state.portfolio_delta + state.perp_position_qty;

    // Sanity ceiling — if book_delta has blown up by orders of magnitude past
    // the hedge threshold, something is structurally wrong (unit mismatch,
    // bad fill, runaway position) and we should NOT fire another hedge into
    // a potential feedback loop. Latch the strategy off, log loudly, and
    // require a process restart + human review to resume. This is the
    // guardrail that would have caught the 2026-05-15 unit-cascade at
    // step 3 (fake book_delta ~100 BTC vs ceiling 20 × 0.05 = 1 BTC)
    // before the loop could compound.
    const double ceiling = book_delta_sanity_ceiling_mult_ * max_hedge_abs_delta_;
    if (std::abs(book_delta) > ceiling) {
        if (!risk_halted_) {
            bpt::common::log::error(
                "[OptionsMaker] book_delta={:.4f} exceeds sanity ceiling {:.4f} "
                "(= {:.1f}x max_hedge_abs_delta). HALTING strategy — restart to clear.",
                book_delta,
                ceiling,
                book_delta_sanity_ceiling_mult_);
            risk_halted_ = true;
        }
        return;
    }

    if (std::abs(book_delta) <= max_hedge_abs_delta_)
        return;

    if (state.perp_bid <= 0.0 || state.perp_ask <= 0.0 || state.perp_ask < state.perp_bid)
        return;

    // We need to move book delta by −book_delta. Positive book_delta means
    // we're net long → sell perp. Negative → buy perp.
    const double hedge_qty_native = std::abs(book_delta);  // in underlying units (BTC, ETH)
    const bool sell = (book_delta > 0.0);

    // Cross the BBO by `aggress_bps` so the IOC actually fills. Going through
    // the spread is the cost of synchronous hedging; the option-side
    // half-spread is sized to cover this in expectation.
    const double cross = hedge_aggress_bps_ * 1e-4;
    const double price = sell ? state.perp_bid * (1.0 - cross) : state.perp_ask * (1.0 + cross);

    // Unit conversion: book_delta and hedge_qty_native live in BTC/ETH (the
    // option delta unit). Deribit perp's `amount` field is in USD-equivalent
    // notional (per Deribit API: "for perpetual and inverse futures the
    // amount is in USD units"). Multiply by price to get USD value; the
    // OrderManager rounds to refdata's lot_size (10 for BTC-PERP).
    //
    // 2026-05-15 post-mortem on the unit cascade: earlier code divided by
    // contract_size which produced "number of contracts", but Deribit takes
    // USD value directly. Multiplying by price (not dividing by
    // contract_size) is the right transform.
    //
    // TODO venue-aware: OKX BTC-USDT-SWAP amount is in 0.01-BTC contracts,
    // not USD. When OKX options exist this path needs an adapter-level
    // unit hook.
    const double hedge_qty_for_order = hedge_qty_native * price;

    const uint64_t oid = order_mgr_
                             ->send_new_order(order::NewOrderRequest{
                                 .instrument_id = state.perp_instrument_id,
                                 .exchange_id = state.exchange_id,
                                 .side = sell ? bpt::messages::OrderSide::SELL : bpt::messages::OrderSide::BUY,
                                 .type = bpt::messages::OrderType::LIMIT,
                                 .tif = bpt::messages::TimeInForce::IOC,
                                 .price = price,
                                 .qty = hedge_qty_for_order,
                             })
                             .order_id();
    if (oid == 0)
        return;

    state.perp_order_id = oid;
    state.last_hedge_ns = now_ns;
    order_to_key_[oid] = state_key(state.exchange_id, state.underlying);
    order_is_perp_hedge_[oid] = true;
    live_orders_[oid] = OrderRef{state.exchange_id, state.perp_instrument_id};

    bpt::common::log::info(
        "[OptionsMaker] hedge {} {} qty_native={:.6f} qty_contracts={:.2f} price={:.2f} "
        "book_delta={:.6f} (opts={:.6f} + perp_pos={:.6f})",
        state.underlying,
        sell ? "SELL" : "BUY",
        hedge_qty_native,
        hedge_qty_for_order,
        price,
        book_delta,
        state.portfolio_delta,
        state.perp_position_qty);
}

}  // namespace bpt::strategy::strategy

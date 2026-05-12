#include "strategy/strategy/short_vol_strategy.h"

#include "strategy/clock/sim_clock.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OptionSide.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>

#include <cmath>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectSource;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

static ExchangeId::Value exchange_from_str(const std::string& s) {
    if (s == "BINANCE")
        return ExchangeId::BINANCE;
    if (s == "OKX")
        return ExchangeId::OKX;
    if (s == "HYPERLIQUID")
        return ExchangeId::HYPERLIQUID;
    if (s == "DERIBIT")
        return ExchangeId::DERIBIT;
    return ExchangeId::NULL_VALUE;
}

ShortVolStrategy::ShortVolStrategy(uint64_t correlation_id,
                                   const config::StrategyConfig& cfg,
                                   refdata::IRefdataClient& refdata,
                                   md::IMdClient* md,
                                   order::OrderManager* order_mgr,
                                   vol::VolSurfaceClient* vol_client)
    : correlation_id_(correlation_id),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      risk_(cfg.risk),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr),
      vol_client_(vol_client) {
    const auto& p = cfg.params;

    iv_rv_threshold_ = p["iv_rv_threshold"].value<double>().value_or(0.05);
    iv_rv_exit_threshold_ = p["iv_rv_exit_threshold"].value<double>().value_or(0.02);
    max_portfolio_delta_ = p["max_portfolio_delta"].value<double>().value_or(0.5);
    max_portfolio_vega_ = p["max_portfolio_vega"].value<double>().value_or(10000.0);
    max_portfolio_gamma_ = p["max_portfolio_gamma"].value<double>().value_or(100.0);
    target_notional_usd_ = p["target_notional_usd"].value<double>().value_or(1000.0);
    min_time_to_expiry_ = p["min_time_to_expiry"].value<double>().value_or(1.0) / 365.0;
    max_time_to_expiry_ = p["max_time_to_expiry"].value<double>().value_or(30.0) / 365.0;
    min_abs_delta_ = p["min_abs_delta"].value<double>().value_or(0.15);
    max_abs_delta_ = p["max_abs_delta"].value<double>().value_or(0.50);
    eval_interval_ns_ = static_cast<uint64_t>(p["eval_interval_s"].value<double>().value_or(5.0) * 1e9);
    aggress_bps_ = p["aggress_bps"].value<double>().value_or(5.0);
    rv_window_ = static_cast<size_t>(p["rv_window"].value<int64_t>().value_or(100));
    rv_sample_interval_ns_ = static_cast<uint64_t>(p["rv_sample_interval_s"].value<double>().value_or(60.0) * 1e9);

    if (auto* arr = p["underlyings"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                underlyings_.push_back(*v);
    }

    bpt::common::log::info(
        "[ShortVol] iv_rv_threshold={:.3f} exit={:.3f} delta_range=[{:.2f},{:.2f}] "
        "expiry_range=[{:.1f}d,{:.1f}d] eval_interval={:.1f}s rv_window={}",
        iv_rv_threshold_,
        iv_rv_exit_threshold_,
        min_abs_delta_,
        max_abs_delta_,
        min_time_to_expiry_ * 365.0,
        max_time_to_expiry_ * 365.0,
        static_cast<double>(eval_interval_ns_) / 1e9,
        rv_window_);
}

void ShortVolStrategy::start() {
    bpt::common::log::info("[ShortVol] Strategy started");

    // Subscribe to perp BBO for delta-hedging + option BBO for Pricer vol surface computation.
    // Pricer passively reads stream 2002, so we subscribe to options here to drive MdGateway.
    if (md_client_) {
        std::vector<md::IMdClient::InstrumentDesc> subs;
        for (const auto& [key, state] : states_) {
            if (state.perp_instrument_id != 0) {
                auto inst = refdata_.cache().get(state.perp_instrument_id);
                if (inst) {
                    subs.push_back({state.perp_instrument_id, inst->exchange, inst->symbol});
                    bpt::common::log::info("[ShortVol] Subscribing perp BBO: {} id={} symbol={}",
                                   state.underlying,
                                   state.perp_instrument_id,
                                   inst->symbol);
                }
            }
        }

        // Subscribe to all option instruments so MdGateway publishes their BBOs on stream 2002.
        // Pricer reads these passively to compute implied vol surfaces.
        const auto all = refdata_.cache().get_all();
        int option_count = 0;
        for (const auto& inst : all) {
            if (inst.type != refdata::InstrumentType::OPTION)
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

            subs.push_back({inst.instrument_id, inst.exchange, inst.symbol});
            ++option_count;
        }
        bpt::common::log::info("[ShortVol] Subscribing {} option instruments for vol surface", option_count);

        if (!subs.empty()) {
            md_client_->subscribe(correlation_id_, subs);
        }
    }
}

void ShortVolStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    // Discover perp instruments for each underlying we want to trade.
    // Option instruments are discovered dynamically from VolSurface messages.
    const auto all = cache.get_all();

    for (const auto& inst : all) {
        // Only interested in perps for delta-hedging.
        if (inst.type != refdata::InstrumentType::PERPETUAL)
            continue;

        // Check if this perp's underlying is one we trade.
        bool relevant = underlyings_.empty();
        for (const auto& u : underlyings_) {
            if (inst.base_currency == u) {
                relevant = true;
                break;
            }
        }
        if (!relevant)
            continue;

        // Check exchange filter.
        if (!md_exchanges_.empty()) {
            bool found = false;
            for (const auto& e : md_exchanges_) {
                if (e == inst.exchange) {
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
        }

        auto ex_id = exchange_from_str(inst.exchange);
        auto key = state_key(ex_id, inst.base_currency);
        auto& state = states_[key];
        state.underlying = inst.base_currency;
        state.exchange_id = ex_id;
        state.perp_instrument_id = inst.instrument_id;
        state.perp_tick_size = inst.tick_size;
        state.perp_lot_size = inst.lot_size;
        state.rv_estimator = RealizedVolEstimator(rv_window_, rv_sample_interval_ns_);

        instrument_to_key_[inst.instrument_id] = key;

        bpt::common::log::info("[ShortVol] Discovered perp for {}: id={} exchange={} tick={} lot={}",
                       inst.base_currency,
                       inst.instrument_id,
                       inst.exchange,
                       inst.tick_size,
                       inst.lot_size);
    }

    // Also discover option instruments for our lookups.
    for (const auto& inst : all) {
        if (inst.type != refdata::InstrumentType::OPTION)
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

        auto ex_id = exchange_from_str(inst.exchange);
        auto key = state_key(ex_id, inst.base_currency);
        if (states_.find(key) == states_.end())
            continue;

        instrument_to_key_[inst.instrument_id] = key;
    }

    bpt::common::log::info("[ShortVol] Snapshot processed: {} underlyings tracked", states_.size());
}

void ShortVolStrategy::on_delta(const refdata::Instrument& inst,
                                bpt::messages::DeltaUpdateType::Value /*update_type*/) {
    if (inst.type == refdata::InstrumentType::PERPETUAL || inst.type == refdata::InstrumentType::OPTION) {
        bool relevant = underlyings_.empty();
        for (const auto& u : underlyings_) {
            if (inst.base_currency == u) {
                relevant = true;
                break;
            }
        }
        if (relevant) {
            auto ex_id = exchange_from_str(inst.exchange);
            auto key = state_key(ex_id, inst.base_currency);
            if (states_.find(key) != states_.end()) {
                instrument_to_key_[inst.instrument_id] = key;
            }
        }
    }
}

void ShortVolStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = instrument_to_key_.find(tick.instrumentId());
    if (it == instrument_to_key_.end())
        return;

    auto state_it = states_.find(it->second);
    if (state_it == states_.end())
        return;

    auto& state = state_it->second;
    if (tick.instrumentId() != state.perp_instrument_id)
        return;

    const double bid = tick.bidPrice();
    const double ask = tick.askPrice();

    state.perp_bid = bid;
    state.perp_ask = ask;
    state.perp_last_bbo_ns = tick.timestampNs();

    const double mid = (bid + ask) * 0.5;
    state.rv_estimator.update(mid, tick.timestampNs());
}

void ShortVolStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {
    // Not used — we rely on BBO for RV estimation.
}

void ShortVolStrategy::on_vol_surface(bpt::messages::VolSurface& surface) {
    // getUnderlyingAsString() trims null-padding from the fixed-length SBE field.
    auto underlying = surface.getUnderlyingAsString();
    auto key = state_key(surface.exchangeId(), underlying);

    auto state_it = states_.find(key);
    if (state_it == states_.end()) {
        bpt::common::log::info("[ShortVol] VolSurface key not found: {}", key);
        return;
    }
    auto& state = state_it->second;

    // Update option legs from the surface points.
    int point_count = 0;
    auto& points = surface.points();
    while (points.hasNext()) {
        ++point_count;
        auto& pt = points.next();

        auto& leg = state.options[pt.instrumentId()];
        leg.instrument_id = pt.instrumentId();
        leg.expiry_date = pt.expiryDate();
        leg.strike = pt.strikePrice();
        leg.is_call = (pt.optionSide() == bpt::messages::OptionSide::CALL);
        leg.iv = pt.impliedVol();
        leg.bid_price = pt.bidPrice();
        leg.ask_price = pt.askPrice();
        leg.delta = pt.delta();
        leg.gamma = pt.gamma();
        leg.vega = pt.vega();
        leg.theta = pt.theta();
        leg.forward_price = pt.forwardPrice();
        leg.time_to_expiry = pt.timeToExpiry();

        instrument_to_key_[pt.instrumentId()] = key;
    }

    bpt::common::log::info("[ShortVol] VolSurface {} options={} rv_ready={} perp_bid={:.2f}",
                   state.underlying,
                   point_count,
                   state.rv_estimator.ready(),
                   state.perp_bid);

    recompute_greeks(state);

    const uint64_t now_ns = surface.timestampNs();
    if (now_ns - state.last_eval_ns >= eval_interval_ns_) {
        bpt::common::log::info("[ShortVol] Evaluating {} rv={:.4f}",
                       state.underlying,
                       state.rv_estimator.ready() ? state.rv_estimator.realized_vol() : 0.0);
        evaluate(state, now_ns);
        state.last_eval_ns = now_ns;
    }
}

void ShortVolStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t oid = rpt.orderId();

    auto key_it = order_to_key_.find(oid);
    if (key_it == order_to_key_.end())
        return;

    auto state_it = states_.find(key_it->second);
    if (state_it == states_.end())
        return;
    auto& state = state_it->second;

    const bool is_perp = order_is_perp_.count(oid) && order_is_perp_[oid];

    const auto status = rpt.status();

    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        const double fill_qty = static_cast<double>(rpt.filledQty()) / 1e8;
        const double fill_price = static_cast<double>(rpt.price()) / 1e8;
        const bool is_buy = (rpt.side() == bpt::messages::OrderSide::BUY);
        const double signed_qty = is_buy ? fill_qty : -fill_qty;

        if (is_perp) {
            state.perp_position_qty += signed_qty;
            bpt::common::log::info("[ShortVol] Perp fill: {} {} qty={:.6f} price={:.2f} pos={:.6f}",
                           state.underlying,
                           is_buy ? "BUY" : "SELL",
                           fill_qty,
                           fill_price,
                           state.perp_position_qty);
        } else {
            const uint64_t inst_id = rpt.instrumentId();
            auto opt_it = state.options.find(inst_id);
            if (opt_it != state.options.end()) {
                auto& leg = opt_it->second;
                leg.position_qty += signed_qty;
                leg.entry_price = fill_price;
                bpt::common::log::info("[ShortVol] Option fill: {} {} K={:.0f} qty={:.6f} price={:.4f} pos={:.6f}",
                               state.underlying,
                               is_buy ? "BUY" : "SELL",
                               leg.strike,
                               fill_qty,
                               fill_price,
                               leg.position_qty);
            }

            recompute_greeks(state);
            rebalance_hedge(state, rpt.timestampNs());
        }
    }

    if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gateway_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (gateway_reject)
            bpt::common::log::error("[ShortVol] {} order_id={} REJECTED reason={} source={}",
                            state.underlying,
                            oid,
                            bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                            bpt::messages::RejectSource::c_str(src));
        else
            bpt::common::log::warn("[ShortVol] {} order_id={} REJECTED reason={} source={}",
                           state.underlying,
                           oid,
                           bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                           bpt::messages::RejectSource::c_str(src));
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED) {
        if (is_perp) {
            state.perp_order_id = 0;
        } else {
            const uint64_t inst_id = rpt.instrumentId();
            auto opt_it = state.options.find(inst_id);
            if (opt_it != state.options.end()) {
                opt_it->second.order_id = 0;
            }
        }
        order_to_key_.erase(oid);
        order_is_perp_.erase(oid);
    }
}

void ShortVolStrategy::evaluate(UnderlyingState& state, uint64_t now_ns) {
    if (!order_mgr_) {
        bpt::common::log::debug("[ShortVol] eval: no order_mgr");
        return;
    }
    if (state.perp_bid <= 0.0 || state.perp_ask <= 0.0) {
        return;
    }
    if (!state.rv_estimator.ready()) {
        return;
    }

    const double rv = state.rv_estimator.realized_vol();

    static uint64_t eval_detail_count = 0;
    bool log_detail = (++eval_detail_count <= 5);

    for (auto& [inst_id, leg] : state.options) {
        if (leg.order_id != 0)
            continue;

        if (leg.time_to_expiry < min_time_to_expiry_ || leg.time_to_expiry > max_time_to_expiry_) {
            if (log_detail)
                bpt::common::log::info("[ShortVol] eval skip {}: T={:.2f}d outside [{:.1f},{:.1f}]",
                               inst_id,
                               leg.time_to_expiry * 365.0,
                               min_time_to_expiry_ * 365.0,
                               max_time_to_expiry_ * 365.0);
            continue;
        }

        const double abs_delta = std::abs(leg.delta);
        if (abs_delta < min_abs_delta_ || abs_delta > max_abs_delta_) {
            if (log_detail)
                bpt::common::log::info("[ShortVol] eval skip {}: delta={:.4f} outside [{:.2f},{:.2f}]",
                               inst_id,
                               leg.delta,
                               min_abs_delta_,
                               max_abs_delta_);
            continue;
        }

        const double iv_rv_spread = leg.iv - rv;

        if (leg.position_qty == 0.0) {
            // --- ENTRY: sell option if IV > RV by threshold ---
            if (iv_rv_spread < iv_rv_threshold_)
                continue;

            if (std::abs(state.portfolio_vega) >= max_portfolio_vega_)
                continue;
            if (std::abs(state.portfolio_gamma) >= max_portfolio_gamma_)
                continue;

            const double sell_price = leg.bid_price;
            if (sell_price <= 0.0)
                continue;

            double qty = target_notional_usd_ / (sell_price * leg.forward_price);
            if (qty <= 0.0)
                continue;

            const double spread = leg.ask_price - leg.bid_price;
            const double aggress_offset = spread * aggress_bps_ / 10000.0;
            const double limit_price = leg.bid_price - aggress_offset;

            bpt::common::log::info(
                "[ShortVol] SELL {} {} K={:.0f} iv={:.3f} rv={:.3f} spread={:.3f} "
                "qty={:.6f} price={:.4f}",
                state.underlying,
                leg.is_call ? "CALL" : "PUT",
                leg.strike,
                leg.iv,
                rv,
                iv_rv_spread,
                qty,
                limit_price);

            send_option_order(state, leg, bpt::messages::OrderSide::SELL, qty, limit_price);

        } else if (leg.position_qty < 0.0) {
            // --- EXIT: buy back if IV-RV spread has narrowed ---
            if (iv_rv_spread > iv_rv_exit_threshold_)
                continue;

            const double buy_price = leg.ask_price;
            if (buy_price <= 0.0)
                continue;

            const double spread = leg.ask_price - leg.bid_price;
            const double aggress_offset = spread * aggress_bps_ / 10000.0;
            const double limit_price = leg.ask_price + aggress_offset;
            const double qty = std::abs(leg.position_qty);

            bpt::common::log::info(
                "[ShortVol] BUY-TO-CLOSE {} {} K={:.0f} iv={:.3f} rv={:.3f} "
                "qty={:.6f} price={:.4f}",
                state.underlying,
                leg.is_call ? "CALL" : "PUT",
                leg.strike,
                leg.iv,
                rv,
                qty,
                limit_price);

            send_option_order(state, leg, bpt::messages::OrderSide::BUY, qty, limit_price);
        }
    }

    rebalance_hedge(state, now_ns);
}

void ShortVolStrategy::rebalance_hedge(UnderlyingState& state, uint64_t /*now_ns*/) {
    if (!order_mgr_)
        return;
    if (state.perp_instrument_id == 0)
        return;
    if (state.perp_order_id != 0)
        return;
    if (state.perp_bid <= 0.0 || state.perp_ask <= 0.0)
        return;

    // Target hedge: offset portfolio delta with perp.
    const double target_perp = -state.portfolio_delta;
    const double delta_to_trade = target_perp - state.perp_position_qty;

    if (std::abs(delta_to_trade) < max_portfolio_delta_)
        return;

    const double lot = state.perp_lot_size > 0.0 ? state.perp_lot_size : 1.0;
    double qty = std::floor(std::abs(delta_to_trade) / lot) * lot;
    if (qty <= 0.0)
        return;

    bpt::messages::OrderSide::Value side;
    double price;
    const double spread = state.perp_ask - state.perp_bid;
    if (delta_to_trade > 0.0) {
        side = bpt::messages::OrderSide::BUY;
        price = state.perp_ask + spread * aggress_bps_ / 10000.0;
    } else {
        side = bpt::messages::OrderSide::SELL;
        price = state.perp_bid - spread * aggress_bps_ / 10000.0;
    }

    if (state.perp_tick_size > 0.0) {
        price = std::round(price / state.perp_tick_size) * state.perp_tick_size;
    }

    bpt::common::log::info(
        "[ShortVol] HEDGE {} perp {} qty={:.6f} price={:.2f} "
        "portfolio_delta={:.4f} target_perp={:.4f}",
        state.underlying,
        delta_to_trade > 0.0 ? "BUY" : "SELL",
        qty,
        price,
        state.portfolio_delta,
        target_perp);

    send_perp_order(state, side, qty, price);
}

void ShortVolStrategy::send_option_order(UnderlyingState& state,
                                         OptionLeg& leg,
                                         bpt::messages::OrderSide::Value side,
                                         double qty,
                                         double price) {
    if (!order_mgr_)
        return;

    const uint64_t oid = order_mgr_->place_order(leg.instrument_id,
                                                 state.exchange_id,
                                                 side,
                                                 bpt::messages::OrderType::LIMIT,
                                                 bpt::messages::TimeInForce::IOC,
                                                 price,
                                                 qty);
    if (oid == 0)
        return;

    leg.order_id = oid;
    auto key = state_key(state.exchange_id, state.underlying);
    order_to_key_[oid] = key;
    order_is_perp_[oid] = false;
}

void ShortVolStrategy::send_perp_order(UnderlyingState& state,
                                       bpt::messages::OrderSide::Value side,
                                       double qty,
                                       double price) {
    if (!order_mgr_)
        return;

    const uint64_t oid = order_mgr_->place_order(state.perp_instrument_id,
                                                 state.exchange_id,
                                                 side,
                                                 bpt::messages::OrderType::LIMIT,
                                                 bpt::messages::TimeInForce::IOC,
                                                 price,
                                                 qty);
    if (oid == 0)
        return;

    state.perp_order_id = oid;
    auto key = state_key(state.exchange_id, state.underlying);
    order_to_key_[oid] = key;
    order_is_perp_[oid] = true;
}

void ShortVolStrategy::recompute_greeks(UnderlyingState& state) {
    state.portfolio_delta = 0.0;
    state.portfolio_gamma = 0.0;
    state.portfolio_vega = 0.0;
    state.portfolio_theta = 0.0;

    for (const auto& [id, leg] : state.options) {
        if (leg.position_qty == 0.0)
            continue;
        state.portfolio_delta += leg.delta * leg.position_qty;
        state.portfolio_gamma += leg.gamma * leg.position_qty;
        state.portfolio_vega += leg.vega * leg.position_qty;
        state.portfolio_theta += leg.theta * leg.position_qty;
    }
}

PortfolioState ShortVolStrategy::get_portfolio_state() {
    PortfolioState ps;
    ps.timestamp_ns = bpt::strategy::clock::SimClock::now_ns();

    for (const auto& [key, state] : states_) {
        // Aggregate Greeks
        ps.portfolio_delta += state.portfolio_delta;
        ps.portfolio_gamma += state.portfolio_gamma;
        ps.portfolio_vega  += state.portfolio_vega;
        ps.portfolio_theta += state.portfolio_theta;

        // Option legs
        for (const auto& [id, opt] : state.options) {
            PortfolioState::Leg leg;
            leg.instrument_id = opt.instrument_id;
            leg.underlying = state.underlying;
            leg.expiry_date = opt.expiry_date;
            leg.strike = opt.strike;
            leg.is_call = opt.is_call;
            leg.is_option = true;
            leg.qty = opt.position_qty;
            leg.entry_price = opt.entry_price;
            leg.mark_price = (opt.bid_price + opt.ask_price) / 2.0;
            leg.iv = opt.iv;
            leg.delta = opt.delta;
            leg.gamma = opt.gamma;
            leg.vega = opt.vega;
            leg.theta = opt.theta;
            leg.unrealized_pnl = (leg.mark_price - opt.entry_price) * opt.position_qty;

            if (auto inst = refdata_.cache().get(opt.instrument_id))
                leg.symbol = inst->symbol;

            ps.legs.push_back(std::move(leg));
            ps.total_unrealized_pnl += ps.legs.back().unrealized_pnl;

            // Surface point for this leg
            PortfolioState::SurfacePoint sp;
            sp.instrument_id = opt.instrument_id;
            sp.expiry_date = opt.expiry_date;
            sp.strike = opt.strike;
            sp.is_call = opt.is_call;
            sp.iv = opt.iv;
            sp.delta = opt.delta;
            sp.time_to_expiry = opt.time_to_expiry;
            ps.surface_points.push_back(sp);
        }

        // Perp hedge leg
        if (state.perp_position_qty != 0.0) {
            PortfolioState::Leg perp;
            perp.instrument_id = state.perp_instrument_id;
            perp.underlying = state.underlying;
            perp.is_option = false;
            perp.qty = state.perp_position_qty;
            perp.mark_price = (state.perp_bid + state.perp_ask) / 2.0;
            perp.delta = state.perp_position_qty;

            if (auto inst = refdata_.cache().get(state.perp_instrument_id))
                perp.symbol = inst->symbol;

            ps.legs.push_back(std::move(perp));
        }
    }

    return ps;
}

std::string ShortVolStrategy::state_key(bpt::messages::ExchangeId::Value ex, const std::string& underlying) {
    return std::to_string(static_cast<int>(ex)) + ":" + underlying;
}

}  // namespace bpt::strategy::strategy

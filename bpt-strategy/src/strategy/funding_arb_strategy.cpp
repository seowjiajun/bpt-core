#include "strategy/strategy/funding_arb_strategy.h"

#include "strategy/clock/sim_clock.h"
#include "strategy/refdata/exchange_id.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/InstrumentType.h>
#include <messages/OrderType.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectSource;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

static constexpr double kPriceScale = 1e8;
static constexpr double kQtyScale = 1e5;

const char* FundingArbStrategy::state_name(PairState s) {
    switch (s) {
        case PairState::IDLE:
            return "IDLE";
        case PairState::ENTERING_FIRST_LEG:
            return "ENTERING_FIRST";
        case PairState::ENTERING_SECOND_LEG:
            return "ENTERING_SECOND";
        case PairState::ACTIVE:
            return "ACTIVE";
        case PairState::EXITING_FIRST_LEG:
            return "EXITING_FIRST";
        case PairState::EXITING_SECOND_LEG:
            return "EXITING_SECOND";
        case PairState::UNWINDING:
            return "UNWINDING";
    }
    return "UNKNOWN";
}

// ── Constructor ─────────────────────────────────────────────────────────────

FundingArbStrategy::FundingArbStrategy(uint64_t correlation_id,
                                       const config::StrategyConfig& cfg,
                                       refdata::IRefdataClient& refdata,
                                       md::IMdClient* md,
                                       order::IOrderGatewayClient* order_gw)
    : min_funding_rate_bps_(static_cast<int32_t>(cfg.params["min_funding_rate_bps"].value<int64_t>().value_or(5))),
      exit_funding_rate_bps_(static_cast<int32_t>(cfg.params["exit_funding_rate_bps"].value<int64_t>().value_or(2))),
      min_stable_periods_(static_cast<int>(cfg.params["min_stable_periods"].value<int64_t>().value_or(3))),
      min_time_before_funding_ns_(
          static_cast<uint64_t>(cfg.params["min_time_before_funding_min"].value<double>().value_or(5.0) * 60e9)),
      eval_interval_ns_(static_cast<uint64_t>(cfg.params["eval_interval_s"].value<double>().value_or(30.0) * 1e9)),
      max_basis_loss_bps_(cfg.params["max_basis_loss_bps"].value<double>().value_or(50.0)),
      target_notional_usd_(cfg.params["target_notional_usd"].value<double>().value_or(1000.0)),
      order_timeout_ns_(static_cast<uint64_t>(cfg.params["order_timeout_s"].value<double>().value_or(60.0) * 1e9)),
      aggress_bps_(cfg.params["aggress_bps"].value<double>().value_or(10.0)),
      correlation_id_(correlation_id),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_gw_(order_gw) {
    // Parse base_assets from params.
    if (auto* arr = cfg.params["base_assets"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                base_assets_.push_back(*v);
    }

    // Seed order IDs from timestamp to avoid collisions across restarts.
    // SimClock yields the simulation tick in backtest, wall clock in live —
    // either way the seed avoids overlap between processes / runs sharing
    // an exchange account, while staying reproducible per-tape.
    const uint64_t seed_ns = bpt::strategy::clock::SimClock::now_ns();
    next_order_id_.store(seed_ns / 1'000ULL, std::memory_order_relaxed);  // µs granularity, matches prior behaviour

    bpt::common::log::info(
        "[FA] min_rate={}bps exit_rate={}bps stable_periods={} "
        "eval_interval={}s target_notional=${:.0f} aggress={}bps "
        "max_basis_loss={}bps order_timeout={}s",
        min_funding_rate_bps_,
        exit_funding_rate_bps_,
        min_stable_periods_,
        eval_interval_ns_ / 1'000'000'000ULL,
        target_notional_usd_,
        aggress_bps_,
        max_basis_loss_bps_,
        order_timeout_ns_ / 1'000'000'000ULL);
    bpt::common::log::info("[FA] risk: max_position_usd={} max_order_size_usd={}",
                           cfg.risk.max_position_usd,
                           cfg.risk.max_order_size_usd);
    for (const auto& b : base_assets_)
        bpt::common::log::info("[FA] base asset: {}", b);
    bpt::common::log::info("[FA] order_id seed={}", next_order_id_.load(std::memory_order_relaxed));
}

// ── IStrategy ───────────────────────────────────────────────────────────────

void FundingArbStrategy::start() {
    for (const auto& ex : md_exchanges_)
        bpt::common::log::info("[FA] MD exchange: {}", ex);

    refdata_.subscribe(correlation_id_, CanonicalResolver::build_filters(instruments_, md_exchanges_));
}

void FundingArbStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    bpt::common::log::info("[FA] Snapshot ({} instruments), resolving pairs...", cache.size());
    pairs_.clear();
    instrument_to_base_.clear();
    order_to_base_.clear();
    positions_.clear_all();

    // Resolve all instruments from the snapshot.
    const auto all_ids = CanonicalResolver::resolve(cache, instruments_, md_exchanges_);

    // For each base asset, find the SPOT and PERP instruments.
    for (const auto& base : base_assets_) {
        ArbPair pair;
        pair.base_asset = base;

        for (uint64_t id : all_ids) {
            const auto inst = cache.get(id);
            if (!inst || inst->base_currency != base)
                continue;

            const auto ex_id = refdata::to_exchange_id(inst->exchange);

            LegState leg;
            leg.instrument_id = id;
            leg.symbol = inst->symbol;
            leg.exchange = inst->exchange;
            leg.exchange_id = ex_id;
            leg.tick_size = inst->tick_size;
            leg.lot_size = inst->lot_size;

            if (inst->type == refdata::InstrumentType::SPOT) {
                pair.spot = std::move(leg);
            } else if (inst->type == refdata::InstrumentType::PERPETUAL) {
                pair.perp = std::move(leg);
            }
        }

        if (pair.spot.instrument_id == 0 || pair.perp.instrument_id == 0) {
            bpt::common::log::warn("[FA] {} — missing leg (spot_id={} perp_id={}), skipping",
                                   base,
                                   pair.spot.instrument_id,
                                   pair.perp.instrument_id);
            continue;
        }

        bpt::common::log::info("[FA] {} pair: spot=[{}] {} @ {} tick={} lot={}",
                               base,
                               pair.spot.instrument_id,
                               pair.spot.symbol,
                               pair.spot.exchange,
                               pair.spot.tick_size,
                               pair.spot.lot_size);
        bpt::common::log::info("[FA] {} pair: perp=[{}] {} @ {} tick={} lot={}",
                               base,
                               pair.perp.instrument_id,
                               pair.perp.symbol,
                               pair.perp.exchange,
                               pair.perp.tick_size,
                               pair.perp.lot_size);

        instrument_to_base_[pair.spot.instrument_id] = base;
        instrument_to_base_[pair.perp.instrument_id] = base;
        pairs_.emplace(base, std::move(pair));
    }

    bpt::common::log::info("[FA] Resolved {} pair(s)", pairs_.size());

    if (!md_client_)
        return;

    // Subscribe to MD for all resolved instruments.
    std::vector<md::IMdClient::InstrumentDesc> subs;
    for (const auto& [base, pair] : pairs_) {
        subs.push_back({pair.spot.instrument_id, pair.spot.exchange, pair.spot.symbol});
        subs.push_back({pair.perp.instrument_id, pair.perp.exchange, pair.perp.symbol});
    }
    bpt::common::log::info("[FA] Subscribing MD to {} instrument(s)", subs.size());
    md_client_->subscribe(correlation_id_, subs);
}

void FundingArbStrategy::on_delta(const refdata::Instrument& /*inst*/,
                                  bpt::messages::DeltaUpdateType::Value /*update_type*/) {
    // Funding arb pairs are resolved once at snapshot time.
    // Dynamic instrument adds/removes during a session are not handled —
    // restart Strategy to pick up new instruments.
}

void FundingArbStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {
    // Not used — funding arb is latency-insensitive.
}

// ── BBO ─────────────────────────────────────────────────────────────────────

void FundingArbStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto base_it = instrument_to_base_.find(tick.instrumentId());
    if (base_it == instrument_to_base_.end()) {
        bpt::common::log::info("[FA] BBO tick for unknown instrument_id={}", tick.instrumentId());
        return;
    }

    auto pair_it = pairs_.find(base_it->second);
    if (pair_it == pairs_.end())
        return;

    ArbPair& pair = pair_it->second;
    const uint64_t ts = tick.timestampNs();
    const double bid = tick.bidPrice();
    const double ask = tick.askPrice();

    if (bid <= 0.0 || ask <= 0.0 || ask <= bid)
        return;

    // Update the correct leg.
    if (tick.instrumentId() == pair.spot.instrument_id) {
        pair.spot.bid = bid;
        pair.spot.ask = ask;
        pair.spot.last_bbo_ns = ts;
    } else {
        pair.perp.bid = bid;
        pair.perp.ask = ask;
        pair.perp.last_bbo_ns = ts;
    }

    // Only evaluate when both legs have fresh BBO.
    if (pair.spot.last_bbo_ns == 0 || pair.perp.last_bbo_ns == 0)
        return;

    // Rate-limit evaluation.
    if (ts - pair.last_eval_ts < eval_interval_ns_)
        return;

    evaluate_pair(pair, ts);
    pair.last_eval_ts = ts;
}

// ── Exec Reports ────────────────────────────────────────────────────────────

void FundingArbStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t order_id = rpt.orderId();
    auto base_it = order_to_base_.find(order_id);
    if (base_it == order_to_base_.end())
        return;

    auto pair_it = pairs_.find(base_it->second);
    if (pair_it == pairs_.end())
        return;

    ArbPair& pair = pair_it->second;
    const auto status = rpt.status();

    // Identify which leg this order belongs to.
    LegState* leg = nullptr;
    if (pair.spot.order_id == order_id)
        leg = &pair.spot;
    else if (pair.perp.order_id == order_id)
        leg = &pair.perp;
    else
        return;

    if (status == ExecStatus::ACKED) {
        bpt::common::log::debug("[FA] {} {} ACKED order_id={}", pair.base_asset, leg->symbol, order_id);
        return;
    }

    if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gateway_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (gateway_reject)
            bpt::common::log::error("[FA] {} {} REJECTED reason={} source={} order_id={}",
                                    pair.base_asset,
                                    leg->symbol,
                                    bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                    bpt::messages::RejectSource::c_str(src),
                                    order_id);
        else
            bpt::common::log::warn("[FA] {} {} REJECTED reason={} source={} order_id={}",
                                   pair.base_asset,
                                   leg->symbol,
                                   bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                   bpt::messages::RejectSource::c_str(src),
                                   order_id);
    } else {
        bpt::common::log::info("[FA] {} {} status={} filled={:.6f} price={:.2f} order_id={}",
                               pair.base_asset,
                               leg->symbol,
                               bpt::messages::ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / kPriceScale,
                               static_cast<double>(rpt.price()) / kPriceScale,
                               order_id);
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        positions_.on_fill(leg->instrument_id, leg->exchange_id, rpt.side(), rpt.filledQty(), rpt.price());
        handle_fill(pair, *leg, rpt);
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED) {
        handle_terminal(pair, *leg, status);
        leg->order_id = 0;
        order_to_base_.erase(order_id);
    }
}

// ── Core Logic ──────────────────────────────────────────────────────────────

void FundingArbStrategy::evaluate_pair(ArbPair& pair, uint64_t now_ns) {
    // Read funding rate for the perp leg.
    const auto fr = refdata_.funding_rate_cache().get(pair.perp.instrument_id, now_ns);

    if (pair.state == PairState::IDLE) {
        if (!fr) {
            bpt::common::log::info("[FA] {} IDLE — no funding rate for perp_id={}",
                                   pair.base_asset,
                                   pair.perp.instrument_id);
            return;
        }

        // Track funding rate sign stability.
        const int sign = (fr->rate_bps > 0) ? 1 : (fr->rate_bps < 0) ? -1 : 0;
        if (sign == pair.last_funding_sign && sign != 0) {
            ++pair.stable_sign_count;
        } else {
            pair.stable_sign_count = (sign != 0) ? 1 : 0;
        }
        pair.last_funding_sign = sign;
        pair.last_funding_rate_bps = fr->rate_bps;

        // Entry conditions.
        const bool rate_ok = std::abs(fr->rate_bps) >= min_funding_rate_bps_;
        const bool stable = pair.stable_sign_count >= min_stable_periods_;
        const bool not_too_close =
            (fr->next_funding_ts == 0) || (fr->next_funding_ts > now_ns + min_time_before_funding_ns_);

        bpt::common::log::info("[FA] {} IDLE: rate={}bps |rate|>={}? {} stable={}/{} time_ok={}",
                               pair.base_asset,
                               fr->rate_bps,
                               min_funding_rate_bps_,
                               rate_ok ? "Y" : "N",
                               pair.stable_sign_count,
                               min_stable_periods_,
                               not_too_close ? "Y" : "N");

        if (rate_ok && stable && not_too_close) {
            // direction: +1 if funding positive (longs pay shorts → go long spot / short perp)
            const int direction = (fr->rate_bps > 0) ? 1 : -1;
            bpt::common::log::info("[FA] {} ENTRY signal: rate={}bps stable={} direction={}",
                                   pair.base_asset,
                                   fr->rate_bps,
                                   pair.stable_sign_count,
                                   direction > 0 ? "long_spot/short_perp" : "short_spot/long_perp");
            enter_position(pair, direction, now_ns);
        }
    } else if (pair.state == PairState::ACTIVE) {
        // Exit conditions.
        bool should_exit = false;
        std::string reason;

        if (fr && std::abs(fr->rate_bps) < exit_funding_rate_bps_) {
            should_exit = true;
            reason = "rate_reverted";
        }

        if (fr && ((pair.direction > 0 && fr->rate_bps < 0) || (pair.direction < 0 && fr->rate_bps > 0))) {
            should_exit = true;
            reason = "rate_flipped";
        }

        // Basis risk check.
        const double spot_mid = (pair.spot.bid + pair.spot.ask) * 0.5;
        const double perp_mid = (pair.perp.bid + pair.perp.ask) * 0.5;
        const double current_basis = perp_mid - spot_mid;
        const double entry_basis = pair.perp_entry_mid - pair.spot_entry_mid;
        const double basis_move_bps = std::abs(current_basis - entry_basis) / spot_mid * 10000.0;

        if (basis_move_bps > max_basis_loss_bps_) {
            should_exit = true;
            reason = "basis_stop";
        }

        if (should_exit) {
            bpt::common::log::info("[FA] {} EXIT signal: reason={} rate={}bps basis_move={:.1f}bps",
                                   pair.base_asset,
                                   reason,
                                   fr ? fr->rate_bps : 0,
                                   basis_move_bps);
            exit_position(pair, now_ns);
        } else {
            bpt::common::log::debug("[FA] {} ACTIVE: rate={}bps basis_move={:.1f}bps",
                                    pair.base_asset,
                                    fr ? fr->rate_bps : 0,
                                    basis_move_bps);
        }
    } else if (pair.state == PairState::ENTERING_FIRST_LEG || pair.state == PairState::ENTERING_SECOND_LEG) {
        // Check for order timeout.
        LegState& active_leg = (pair.state == PairState::ENTERING_FIRST_LEG) ? pair.spot : pair.perp;
        if (active_leg.order_id != 0 && now_ns - pair.entry_ts > order_timeout_ns_) {
            bpt::common::log::warn("[FA] {} order timeout in state {} — cancelling order_id={}",
                                   pair.base_asset,
                                   state_name(pair.state),
                                   active_leg.order_id);
            if (order_gw_)
                order_gw_->send_cancel(active_leg.order_id, active_leg.exchange_id, active_leg.instrument_id);
        }
    }
}

void FundingArbStrategy::enter_position(ArbPair& pair, int direction, uint64_t now_ns) {
    const double spot_mid = (pair.spot.bid + pair.spot.ask) * 0.5;
    const uint64_t qty = compute_leg_qty(pair, spot_mid);
    if (qty == 0) {
        bpt::common::log::warn("[FA] {} cannot compute leg quantity", pair.base_asset);
        return;
    }

    pair.direction = direction;
    pair.entry_ts = now_ns;
    pair.spot_entry_mid = spot_mid;
    pair.perp_entry_mid = (pair.perp.bid + pair.perp.ask) * 0.5;

    // Enter spot first (hedging leg).
    const auto spot_side = (direction > 0) ? bpt::messages::OrderSide::BUY : bpt::messages::OrderSide::SELL;
    pair.state = PairState::ENTERING_FIRST_LEG;
    send_leg_order(pair, pair.spot, spot_side, qty);
}

void FundingArbStrategy::exit_position(ArbPair& pair, uint64_t now_ns) {
    pair.entry_ts = now_ns;  // reuse for timeout tracking

    // Exit perp first (stop funding exposure).
    const auto perp_side = (pair.direction > 0) ? bpt::messages::OrderSide::BUY    // close short
                                                : bpt::messages::OrderSide::SELL;  // close long
    const uint64_t qty = static_cast<uint64_t>(std::abs(pair.perp_filled_qty));

    pair.state = PairState::EXITING_FIRST_LEG;
    send_leg_order(pair, pair.perp, perp_side, qty);
}

uint64_t FundingArbStrategy::compute_leg_qty(const ArbPair& pair, double mid) const {
    if (mid <= 0.0)
        return 0;

    // Target quantity in base units.
    const double target_base = target_notional_usd_ / mid;

    // Round to the coarser of the two lot sizes.
    double lot = std::max(pair.spot.lot_size, pair.perp.lot_size);
    if (lot <= 0.0)
        lot = 1.0 / kQtyScale;  // fallback

    const double rounded = std::floor(target_base / lot) * lot;
    if (rounded <= 0.0)
        return 0;

    return static_cast<uint64_t>(std::round(rounded * kQtyScale));
}

void FundingArbStrategy::send_leg_order(ArbPair& pair,
                                        LegState& leg,
                                        bpt::messages::OrderSide::Value side,
                                        uint64_t qty) {
    if (!order_gw_)
        return;

    // Aggressive LIMIT IOC: cross the spread to get an immediate fill.
    double price;
    if (side == OrderSide::BUY) {
        price = leg.ask * (1.0 + aggress_bps_ / 10000.0);
        if (leg.tick_size > 0.0)
            price = std::ceil(price / leg.tick_size) * leg.tick_size;
    } else {
        price = leg.bid * (1.0 - aggress_bps_ / 10000.0);
        if (leg.tick_size > 0.0)
            price = std::floor(price / leg.tick_size) * leg.tick_size;
    }

    const int64_t price_fixed = static_cast<int64_t>(std::round(price * kPriceScale));
    const uint64_t order_id = next_order_id_.fetch_add(1, std::memory_order_relaxed);

    bpt::common::log::info("[FA] {} {} {} {} @ {:.2f} qty={} → order_id={}",
                           pair.base_asset,
                           (side == OrderSide::BUY ? "BUY" : "SELL"),
                           leg.symbol,
                           leg.exchange,
                           price,
                           qty,
                           order_id);

    leg.order_id = order_id;
    order_to_base_[order_id] = pair.base_asset;

    if (!order_gw_->send_new_order(order_id,
                                   leg.exchange_id,
                                   leg.instrument_id,
                                   side,
                                   OrderType::LIMIT,
                                   TimeInForce::IOC,
                                   price_fixed,
                                   qty,
                                   /*exec_inst*/ 0,
                                   leg.symbol)) {
        leg.order_id = 0;
        order_to_base_.erase(order_id);
    }
}

void FundingArbStrategy::handle_fill(ArbPair& pair, const LegState& leg, const bpt::messages::ExecutionReport& rpt) {
    // Track filled qty per leg.
    const int64_t fill_qty = static_cast<int64_t>(rpt.filledQty());
    const bool is_spot = (leg.instrument_id == pair.spot.instrument_id);

    if (is_spot) {
        if (rpt.side() == bpt::messages::OrderSide::BUY)
            pair.spot_filled_qty += fill_qty;
        else
            pair.spot_filled_qty -= fill_qty;
    } else {
        if (rpt.side() == bpt::messages::OrderSide::BUY)
            pair.perp_filled_qty += fill_qty;
        else
            pair.perp_filled_qty -= fill_qty;
    }

    if (const auto pos = positions_.get(leg.instrument_id, leg.exchange_id)) {
        bpt::common::log::info("[FA] {} Position {} net_qty={:.6f} rpnl={:.4f}",
                               pair.base_asset,
                               leg.symbol,
                               static_cast<double>(pos->net_qty) / kPriceScale,
                               pos->realized_pnl);
    }
}

void FundingArbStrategy::handle_terminal(ArbPair& pair, LegState& leg, bpt::messages::ExecStatus::Value status) {
    const bool is_spot = (leg.instrument_id == pair.spot.instrument_id);

    switch (pair.state) {
        case PairState::ENTERING_FIRST_LEG:
            if (status == ExecStatus::FILLED) {
                // First leg filled — send second leg (perp).
                const auto perp_side =
                    (pair.direction > 0) ? bpt::messages::OrderSide::SELL : bpt::messages::OrderSide::BUY;
                const uint64_t qty = compute_leg_qty(pair, (pair.spot.bid + pair.spot.ask) * 0.5);
                pair.state = PairState::ENTERING_SECOND_LEG;
                send_leg_order(pair, pair.perp, perp_side, qty);
            } else {
                // First leg cancelled/rejected — back to IDLE.
                bpt::common::log::warn("[FA] {} first leg {} — returning to IDLE",
                                       pair.base_asset,
                                       bpt::messages::ExecStatus::c_str(status));
                pair.state = PairState::IDLE;
                pair.direction = 0;
            }
            break;

        case PairState::ENTERING_SECOND_LEG:
            if (status == ExecStatus::FILLED) {
                // Both legs filled — position is active.
                pair.state = PairState::ACTIVE;
                bpt::common::log::info("[FA] {} ACTIVE: direction={} spot_qty={} perp_qty={}",
                                       pair.base_asset,
                                       pair.direction > 0 ? "long_spot/short_perp" : "short_spot/long_perp",
                                       pair.spot_filled_qty,
                                       pair.perp_filled_qty);
            } else {
                // Second leg failed — must unwind the first leg.
                bpt::common::log::warn("[FA] {} second leg {} — unwinding first leg",
                                       pair.base_asset,
                                       bpt::messages::ExecStatus::c_str(status));
                pair.state = PairState::UNWINDING;
                const auto unwind_side = (pair.direction > 0) ? bpt::messages::OrderSide::SELL  // close long spot
                                                              : bpt::messages::OrderSide::BUY;  // close short spot
                const uint64_t qty = static_cast<uint64_t>(std::abs(pair.spot_filled_qty));
                send_leg_order(pair, pair.spot, unwind_side, qty);
            }
            break;

        case PairState::EXITING_FIRST_LEG:
            if (status == ExecStatus::FILLED) {
                // Perp closed — now close spot.
                const auto spot_side = (pair.direction > 0) ? bpt::messages::OrderSide::SELL  // close long spot
                                                            : bpt::messages::OrderSide::BUY;  // close short spot
                const uint64_t qty = static_cast<uint64_t>(std::abs(pair.spot_filled_qty));
                pair.state = PairState::EXITING_SECOND_LEG;
                send_leg_order(pair, pair.spot, spot_side, qty);
            } else {
                // Exit first leg failed — retry on next eval.
                bpt::common::log::warn("[FA] {} exit first leg {} — will retry",
                                       pair.base_asset,
                                       bpt::messages::ExecStatus::c_str(status));
                pair.state = PairState::ACTIVE;
            }
            break;

        case PairState::EXITING_SECOND_LEG:
            // Second exit leg terminal — back to IDLE regardless.
            bpt::common::log::info("[FA] {} fully exited: status={}",
                                   pair.base_asset,
                                   bpt::messages::ExecStatus::c_str(status));
            pair.state = PairState::IDLE;
            pair.direction = 0;
            pair.spot_filled_qty = 0;
            pair.perp_filled_qty = 0;
            pair.stable_sign_count = 0;
            break;

        case PairState::UNWINDING:
            // Unwind complete — back to IDLE.
            bpt::common::log::info("[FA] {} unwind complete", pair.base_asset);
            pair.state = PairState::IDLE;
            pair.direction = 0;
            pair.spot_filled_qty = 0;
            pair.perp_filled_qty = 0;
            pair.stable_sign_count = 0;
            break;

        default:
            break;
    }
}

// ── Console strategy-state JSON ───────────────────────────────────────────
//
// One snapshot per call. FundingArb manages N independent (base_asset → pair)
// arbs; the console panel currently shows one, so emit the first non-IDLE
// pair when there is one, else the first pair we've got. Multi-pair display
// is a later panel evolution.

std::string FundingArbStrategy::get_strategy_state_json() {
    if (pairs_.empty())
        return {};

    // Prefer a pair that's actually trading; fall back to the first one.
    const ArbPair* p = nullptr;
    for (const auto& [_, pair] : pairs_) {
        if (pair.state != PairState::IDLE) {
            p = &pair;
            break;
        }
    }
    if (!p)
        p = &pairs_.begin()->second;

    auto leg_status_str = [](PairState s) -> std::string {
        switch (s) {
            case PairState::IDLE:
                return "flat";
            case PairState::ENTERING_FIRST_LEG:
            case PairState::ENTERING_SECOND_LEG:
                return "entering";
            case PairState::ACTIVE:
                return "open";
            case PairState::EXITING_FIRST_LEG:
            case PairState::EXITING_SECOND_LEG:
            case PairState::UNWINDING:
                return "exiting";
        }
        return "flat";
    };

    const double spot_mid = (p->spot.bid + p->spot.ask) * 0.5;
    const double perp_mid = (p->perp.bid + p->perp.ask) * 0.5;
    // Basis is only meaningful when BOTH legs have a real mid. On HL
    // testnet the perp side often has ask=0 (filtered upstream by
    // MdValidator), leaving perp_mid=0 — emitting (0 - spot)/spot then
    // gives a nonsense -10000 bps that pollutes the console. Show 0
    // until both legs warm up; the console differentiates by the
    // perp_mid being 0.
    const double basis_bps = (spot_mid > 0 && perp_mid > 0) ? (perp_mid - spot_mid) / spot_mid * 1e4 : 0.0;
    const double funding_rate = p->last_funding_rate_bps / 1e4;  // bps → decimal
    // Hyperliquid funding is hourly (24/day, 8760/year). OKX is 8-hourly
    // (3/day, 1095/year). Strategy doesn't track which schedule; use the
    // conservative 8-hourly assumption for now. Refinement: include the
    // schedule in the JSON so the panel can label correctly.
    constexpr double kFundingPeriodsPerYear = 1095.0;
    const double funding_apr = funding_rate * kFundingPeriodsPerYear;
    // Position size in base units. Filled qty is 1e5 fixed-point.
    const double spot_qty = static_cast<double>(p->spot_filled_qty) / 1e5;
    const double perp_qty = static_cast<double>(p->perp_filled_qty) / 1e5;

    nlohmann::json j;
    j["type"] = "strategyState";
    j["kind"] = "FundingArb";
    // base_asset is the natural symbol for the console (BTC, ETH, …); the
    // top-bar already shows the configured BPT_BRIDGE_SYMBOL separately.
    j["symbol"] = p->base_asset;
    j["exchange"] = p->perp.exchange;  // perp leg's venue carries the funding
    j["spotPx"] = spot_mid;
    j["perpPx"] = perp_mid;
    j["basisBps"] = basis_bps;
    j["fundingRate"] = funding_rate;
    j["fundingApr"] = funding_apr;
    j["spotQty"] = spot_qty;
    j["perpQty"] = perp_qty;
    j["hedgedDelta"] = spot_qty + perp_qty;
    j["legStatus"] = leg_status_str(p->state);
    return j.dump();
}

}  // namespace bpt::strategy::strategy

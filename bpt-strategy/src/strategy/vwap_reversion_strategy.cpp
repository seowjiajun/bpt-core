#include "strategy/strategy/vwap_reversion_strategy.h"

#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/InstrumentType.h>
#include <messages/OrderType.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>

#include <algorithm>
#include <cmath>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectSource;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

static constexpr double kOrderQty = 0.001;  // 0.001 base unit in natural units

VwapReversionStrategy::VwapReversionStrategy(uint64_t correlation_id,
                                             const config::StrategyConfig& cfg,
                                             refdata::IRefdataClient& refdata,
                                             md::IMdClient* md,
                                             order::OrderManager* order_mgr)
    : correlation_id_(correlation_id),
      ema_period_(static_cast<std::size_t>(cfg.params["vwap_window_trades"].value<int64_t>().value_or(200))),
      min_bbo_ticks_(static_cast<std::size_t>(cfg.params["min_trades_to_signal"].value<int64_t>().value_or(50))),
      ema_alpha_(2.0 / (static_cast<double>(ema_period_) + 1.0)),
      entry_threshold_(cfg.params["entry_threshold"].value<double>().value_or(0.002)),
      exit_threshold_(cfg.params["exit_threshold"].value<double>().value_or(0.0005)),
      stop_threshold_(cfg.params["stop_threshold"].value<double>().value_or(0.005)),
      cooldown_ns_(static_cast<uint64_t>(cfg.params["cooldown_ms"].value<int64_t>().value_or(2000)) * 1'000'000ULL),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    bpt::common::log::info(
        "[VwapReversion] ema_period={} alpha={:.6f} min_bbo={} entry={:.4f}% exit={:.4f}% "
        "stop={:.4f}% cooldown_ms={}",
        ema_period_,
        ema_alpha_,
        min_bbo_ticks_,
        entry_threshold_ * 100.0,
        exit_threshold_ * 100.0,
        stop_threshold_ * 100.0,
        cooldown_ns_ / 1'000'000ULL);
    bpt::common::log::info("[VwapReversion] risk: max_position_usd={} max_order_size_usd={}",
                           cfg.risk.max_position_usd,
                           cfg.risk.max_order_size_usd);
    for (const auto& s : instruments_)
        bpt::common::log::info("[VwapReversion] instrument: {}", s);
}

// ── IStrategy ──────────────────────────────────────────────────────────────

void VwapReversionStrategy::start() {
    for (const auto& ex : md_exchanges_)
        bpt::common::log::info("[VwapReversion] MD exchange: {}", ex);

    refdata_.subscribe(correlation_id_, CanonicalResolver::build_filters(instruments_, md_exchanges_));
}

void VwapReversionStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    bpt::common::log::info("[VwapReversion] Snapshot ({} instruments), resolving universe...", cache.size());
    state_.clear();
    order_to_instrument_.clear();
    positions_.clear_all();

    for (const auto& r : CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_)) {
        state_.emplace(r.instrument_id,
                       InstrumentState{.symbol = r.instrument.symbol,
                                       .exchange = r.instrument.exchange,
                                       .exchange_id = r.exchange_id});
        bpt::common::log::info("  [{}] {} @ {}", r.instrument_id, r.instrument.symbol, r.instrument.exchange);
    }

    bpt::common::log::info("[VwapReversion] Trading universe: {} instrument(s)", state_.size());

    if (!md_client_)
        return;
    auto subs = md::build_subscriptions(state_);
    bpt::common::log::info("[VwapReversion] Subscribing MD to {} instrument(s)", subs.size());
    md_client_->subscribe(correlation_id_, subs);
}

void VwapReversionStrategy::on_delta(const refdata::Instrument& inst,
                                     bpt::messages::DeltaUpdateType::Value update_type) {
    if (update_type == bpt::messages::DeltaUpdateType::ADD) {
        if (!CanonicalResolver::matches(instruments_, md_exchanges_, inst))
            return;

        const auto ex_id = refdata::to_exchange_id(inst.exchange);
        state_.emplace(inst.instrument_id,
                       InstrumentState{.symbol = inst.symbol, .exchange = inst.exchange, .exchange_id = ex_id});
        bpt::common::log::info("[VwapReversion] Delta ADD {} @ {}", inst.symbol, inst.exchange);

    } else if (update_type == bpt::messages::DeltaUpdateType::REMOVE) {
        state_.erase(inst.instrument_id);
        bpt::common::log::info("[VwapReversion] Delta REMOVE {} @ {}", inst.symbol, inst.exchange);
    }
}

void VwapReversionStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {
    // Trade ticks not used — signal is derived from BBO mid-price EMA.
}

void VwapReversionStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;

    const double mid = (tick.bidPrice() + tick.askPrice()) * 0.5;
    if (mid <= 0.0)
        return;

    // Update EMA; seed with first mid on tick 0
    if (st.bbo_count == 0)
        st.ema_mid = mid;
    else
        st.ema_mid = ema_alpha_ * mid + (1.0 - ema_alpha_) * st.ema_mid;
    ++st.bbo_count;

    // Need enough BBO ticks to trust the EMA
    if (st.bbo_count < min_bbo_ticks_)
        return;

    const double vwap = st.ema_mid;  // variable kept as "vwap" so send_order logging stays intact

    const int64_t net = positions_.net_qty(tick.instrumentId(), st.exchange_id);
    const bool has_position = net != 0;
    const bool has_order = st.open_order_id != 0;

    if (!has_position && !has_order) {
        // ── Entry ─────────────────────────────────────────────────────────
        if (tick.timestampNs() < st.last_signal_ns + cooldown_ns_)
            return;

        const double dev = (mid - vwap) / vwap;
        if (dev > +entry_threshold_) {
            send_order(tick.instrumentId(),
                       st,
                       bpt::messages::OrderSide::SELL,
                       mid,
                       vwap,
                       kOrderQty,
                       tick.timestampNs(),
                       "ENTRY SHORT");
        } else if (dev < -entry_threshold_) {
            send_order(tick.instrumentId(),
                       st,
                       bpt::messages::OrderSide::BUY,
                       mid,
                       vwap,
                       kOrderQty,
                       tick.timestampNs(),
                       "ENTRY LONG");
        }

    } else if (has_position && !has_order) {
        // ── Exit / stop ───────────────────────────────────────────────────
        const auto pos = positions_.get(tick.instrumentId(), st.exchange_id);
        if (!pos)
            return;

        // net_qty accumulates qty_fp values (quantity_natural * 1e8, from order_manager).
        // send_order expects natural units, so divide by 1e8.
        const double close_qty = static_cast<double>(std::abs(net)) / 1e8;

        if (net > 0) {
            // Long: sell to exit when price has reverted up toward VWAP, or stop out
            const bool revert = mid >= vwap * (1.0 - exit_threshold_);
            const bool stop = mid <= pos->avg_price * (1.0 - stop_threshold_);
            if (revert || stop) {
                send_order(tick.instrumentId(),
                           st,
                           bpt::messages::OrderSide::SELL,
                           mid,
                           vwap,
                           close_qty,
                           tick.timestampNs(),
                           stop ? "STOP LONG" : "EXIT LONG");
            }
        } else {
            // Short: buy to exit when price has reverted down toward VWAP, or stop out
            const bool revert = mid <= vwap * (1.0 + exit_threshold_);
            const bool stop = mid >= pos->avg_price * (1.0 + stop_threshold_);
            if (revert || stop) {
                send_order(tick.instrumentId(),
                           st,
                           bpt::messages::OrderSide::BUY,
                           mid,
                           vwap,
                           close_qty,
                           tick.timestampNs(),
                           stop ? "STOP SHORT" : "EXIT SHORT");
            }
        }
    }
}

void VwapReversionStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t order_id = rpt.orderId();
    const uint64_t instrument_id = rpt.instrumentId();
    const auto status = rpt.status();

    auto inst_it = order_to_instrument_.find(order_id);
    if (inst_it == order_to_instrument_.end())
        return;

    // Use the canonical instrument_id stored at order placement — the exec
    // report's instrumentId() may be 0 if the gateway doesn't carry canonical IDs.
    const uint64_t canonical_id = inst_it->second;
    auto state_it = state_.find(canonical_id);
    if (state_it == state_.end())
        return;

    InstrumentState& st = state_it->second;

    if (status == ExecStatus::ACKED) {
        bpt::common::log::debug("[VwapReversion] ExecReport order_id={} {} {} ACKED", order_id, st.symbol, st.exchange);
    } else if (status == ExecStatus::REJECTED) {
        const auto src = rpt.rejectSource();
        const bool gateway_reject = (src == RejectSource::GATEWAY || src == RejectSource::RISK);
        if (gateway_reject)
            bpt::common::log::error("[VwapReversion] ExecReport order_id={} {} {} REJECTED reason={} source={}",
                                    order_id,
                                    st.symbol,
                                    st.exchange,
                                    bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                    bpt::messages::RejectSource::c_str(src));
        else
            bpt::common::log::warn("[VwapReversion] ExecReport order_id={} {} {} REJECTED reason={} source={}",
                                   order_id,
                                   st.symbol,
                                   st.exchange,
                                   bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                                   bpt::messages::RejectSource::c_str(src));
    } else {
        bpt::common::log::info("[VwapReversion] ExecReport order_id={} {} {} status={} filled={:.6f} price={:.2f}",
                               order_id,
                               st.symbol,
                               st.exchange,
                               bpt::messages::ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / 1e8,
                               static_cast<double>(rpt.price()) / 1e8);
    }

    if (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL) {
        positions_.on_fill(canonical_id, st.exchange_id, rpt.side(), rpt.filledQty(), rpt.price());

        if (const auto pos = positions_.get(canonical_id, st.exchange_id)) {
            bpt::common::log::info("[VwapReversion] Position {} @ {}  net_qty={}  avg_price={:.6f}  rpnl={:.4f}",
                                   st.symbol,
                                   st.exchange,
                                   pos->net_qty,
                                   pos->avg_price,
                                   pos->realized_pnl);
        }
    }

    // Terminal statuses: clear the open-order slot
    if (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED) {
        if (st.open_order_id == order_id) {
            st.open_order_id = 0;
            st.open_order_side = bpt::messages::OrderSide::NULL_VALUE;
        }
        order_to_instrument_.erase(order_id);
    }
    // PARTIAL: order still live — keep open_order_id so no duplicate is sent
    // ACKED:   order acknowledged but not yet filled — keep open_order_id
}

// ── Private ────────────────────────────────────────────────────────────────

void VwapReversionStrategy::send_order(uint64_t instrument_id,
                                       InstrumentState& st,
                                       bpt::messages::OrderSide::Value side,
                                       double mid,
                                       double vwap,
                                       double quantity,
                                       uint64_t timestamp_ns,
                                       const char* reason) {
    const auto vex_it = venue_exec_.find(st.exchange);
    if (vex_it == venue_exec_.end() || !vex_it->second.enabled) {
        bpt::common::log::debug("[VwapReversion] Venue {} not enabled — signal suppressed", st.exchange);
        return;
    }
    const auto& vex = vex_it->second;

    const auto order_type = (vex.order_type == "MARKET") ? OrderType::MARKET : OrderType::LIMIT;
    const auto tif = (vex.tif == "IOC") ? TimeInForce::IOC : (vex.tif == "FOK") ? TimeInForce::FOK : TimeInForce::GTC;

    // Price: aggressive limit (0.1% over/under mid) so it crosses the spread;
    // zero for MARKET orders.
    const double price_f = (order_type == OrderType::MARKET) ? 0.0
                           : (side == OrderSide::BUY)        ? mid * 1.001
                                                             : mid * 0.999;

    if (!order_mgr_) {
        bpt::common::log::info("[VwapReversion] {} {} {} @ {} mid={:.6f} vwap={:.6f} (no gateway)",
                               reason,
                               (side == OrderSide::BUY ? "BUY" : "SELL"),
                               st.symbol,
                               st.exchange,
                               mid,
                               vwap);
        return;
    }

    bpt::common::log::debug("[VwapReversion] {} {} {} @ {} mid={:.6f} vwap={:.6f} dev={:+.4f}%",
                            reason,
                            (side == OrderSide::BUY ? "BUY" : "SELL"),
                            st.symbol,
                            st.exchange,
                            mid,
                            vwap,
                            ((mid - vwap) / vwap) * 100.0);

    const uint64_t order_id =
        order_mgr_->place_order(instrument_id, st.exchange_id, side, order_type, tif, price_f, quantity);
    if (order_id != 0) {
        bpt::common::log::debug("[VwapReversion] order placed → order_id={}", order_id);
        st.open_order_id = order_id;
        st.open_order_side = side;
        st.last_signal_ns = timestamp_ns;
        order_to_instrument_[order_id] = instrument_id;
    }
}

}  // namespace bpt::strategy::strategy

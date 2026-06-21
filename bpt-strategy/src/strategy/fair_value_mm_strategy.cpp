#include "strategy/strategy/fair_value_mm_strategy.h"

#include "strategy/clock/sim_clock.h"
#include "strategy/config/fair_value_config.h"
#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"
#include "strategy/strategy/canonical_resolver.h"
#include "strategy/strategy/reconciler.h"
#include "strategy/venue/min_order_value.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <cmath>
#include <nlohmann/json.hpp>

namespace bpt::strategy::strategy {

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::TimeInForce;

namespace {

quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("FVMM");
    return l;
}

}  // namespace

// ── Construction ──────────────────────────────────────────────────────────────

FairValueMmStrategy::FairValueMmStrategy(uint64_t correlation_id,
                                         const config::StrategyConfig& cfg,
                                         refdata::IRefdataClient& refdata,
                                         md::IMdClient* md,
                                         order::OrderManager* order_mgr)
    : correlation_id_(correlation_id),
      vol_halflife_s_(cfg.params["vol_halflife_s"].value<double>().value_or(60.0)),
      vol_warmup_ticks_(static_cast<std::size_t>(cfg.params["vol_warmup_ticks"].value<int64_t>().value_or(20))),
      spread_vol_mult_(cfg.params["spread_vol_mult"].value<double>().value_or(2.0)),
      min_spread_bps_(cfg.params["min_spread_bps"].value<double>().value_or(2.0)),
      max_spread_bps_(cfg.params["max_spread_bps"].value<double>().value_or(20.0)),
      skew_alpha_(cfg.params["skew_alpha"].value<double>().value_or(0.1)),
      one_sided_threshold_(cfg.params["one_sided_threshold"].value<double>().value_or(0.8)),
      requote_threshold_(cfg.params["requote_threshold"].value<double>().value_or(0.0003)),
      max_inventory_(cfg.params["max_inventory"].value<double>().value_or(100.0)),
      order_qty_(cfg.params["order_qty"].value<double>().value_or(1.0)),
      order_qty_fraction_(cfg.params["order_qty_fraction"].value<double>().value_or(0.0)),
      order_qty_min_(cfg.params["order_qty_min"].value<double>().value_or(0.0)),
      max_inventory_fraction_(cfg.params["max_inventory_fraction"].value<double>().value_or(0.0)),
      pause_below_rpnl_usd_(cfg.params["pause_below_rpnl_usd"].value<double>().value_or(0.0)),
      pause_cooldown_s_(cfg.params["pause_cooldown_s"].value<double>().value_or(300.0)),
      unwinder_(positions_,
                *order_mgr,
                {.passive_timeout_s = cfg.params["unwind_passive_timeout_s"].value<double>().value_or(45.0),
                 .step_interval_s = cfg.params["unwind_step_interval_s"].value<double>().value_or(8.0),
                 .cross_bps = cfg.params["shutdown_cross_bps"].value<double>().value_or(20.0),
                 .max_retries =
                     static_cast<uint32_t>(cfg.params["shutdown_max_unwind_retries"].value<int64_t>().value_or(3))}),
      fv_cfg_(config::parse_fv_config(cfg.params)),
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    bpt::common::log::info(kLog(),
                           "[FVMM:{}] min_spread={} bps max_spread={} bps vol_mult={} "
                           "skew_alpha={} one_sided_thr={} requote_thr={}",
                           correlation_id_,
                           min_spread_bps_,
                           max_spread_bps_,
                           spread_vol_mult_,
                           skew_alpha_,
                           one_sided_threshold_,
                           requote_threshold_);
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void FairValueMmStrategy::start() {
    auto filters = CanonicalResolver::build_filters(instruments_, md_exchanges_);
    refdata_.subscribe(correlation_id_, std::move(filters));
    bpt::common::log::info(kLog(), "[FVMM:{}] started", correlation_id_);
}

void FairValueMmStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    state_.clear();
    positions_.clear_all();

    const auto resolved = CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_);
    for (const auto& r : resolved) {
        const auto& inst = r.instrument;
        InstrumentState st{
            .instrument_id = r.instrument_id,
            .ewma_var = EwmaVariance(vol_halflife_s_),
            .tick_size = inst.tick_size,
            .lot_size = inst.lot_size,
            .symbol = inst.symbol,
            .exchange = inst.exchange,
            .exchange_id = r.exchange_id,
            .instrument_type = inst.type,
            .fv = FairValueEstimator(fv_cfg_),
        };
        state_.emplace(r.instrument_id, std::move(st));
        bpt::common::log::info(kLog(),
                               "[FVMM:{}] resolved id={} symbol={} exchange={}",
                               correlation_id_,
                               r.instrument_id,
                               inst.symbol,
                               inst.exchange);
    }

    if (md_client_) {
        auto subs = md::build_subscriptions(state_, 0);
        md_client_->subscribe(correlation_id_, subs);
        bpt::common::log::info(kLog(), "[FVMM:{}] subscribed to {} MD streams", correlation_id_, subs.size());
    }
}

void FairValueMmStrategy::on_delta(const refdata::Instrument& inst, bpt::messages::DeltaUpdateType::Value) {
    const auto it = state_.find(inst.instrument_id);
    if (it == state_.end())
        return;
    it->second.tick_size = inst.tick_size;
    it->second.lot_size = inst.lot_size;
}

void FairValueMmStrategy::on_refdata_stale_changed(bool stale) {
    refdata_stale_ = stale;
    if (stale)
        bpt::common::log::warn(kLog(), "[FVMM:{}] refdata stale — quoting paused", correlation_id_);
    else
        bpt::common::log::info(kLog(), "[FVMM:{}] refdata fresh — quoting resumed", correlation_id_);
}

// ── Market data ───────────────────────────────────────────────────────────────

void FairValueMmStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;
    auto& st = it->second;

    const double bid_px = tick.bidPrice();
    const double ask_px = tick.askPrice();
    const double bid_qty = tick.bidQty();
    const double ask_qty = tick.askQty();
    const uint64_t ts_ns = tick.timestampNs();

    if (bid_px <= 0.0 || ask_px <= 0.0)
        return;

    st.last_market_bid = bid_px;
    st.last_market_ask = ask_px;
    st.last_tick_ns = ts_ns;

    const double mid = 0.5 * (bid_px + ask_px);
    st.ewma_var.update(mid, ts_ns);

    const double fv = st.fv.estimate(bid_px, ask_px, bid_qty, ask_qty);
    if (std::isnan(fv) || fv <= 0.0)
        return;
    st.last_mid = fv;

    if (shutting_down_ || refdata_stale_)
        return;
    if (st.ewma_var.count() < vol_warmup_ticks_)
        return;

    const double net_qty = static_cast<double>(positions_.net_qty(st.instrument_id, st.exchange_id)) / 1e8;
    const auto q = compute_quotes(st, net_qty);
    if (!q)
        return;

    maybe_requote(st, net_qty, *q, ts_ns);
}

// ── Quote computation ─────────────────────────────────────────────────────────

std::optional<FairValueMmStrategy::QuoteTarget> FairValueMmStrategy::compute_quotes(const InstrumentState& st,
                                                                                    double net_qty) const {
    const double fv = st.last_mid;
    if (fv <= 0.0)
        return std::nullopt;

    const double sigma_bps = std::sqrt(st.ewma_var.value()) * 1e4;
    const double half_spread_bps = std::clamp(sigma_bps * spread_vol_mult_, min_spread_bps_, max_spread_bps_);
    const double half_spread = half_spread_bps / 10000.0 * fv;

    const double max_inv = effective_max_inventory(st);
    const double q_norm = (max_inv > 0.0) ? std::clamp(net_qty / max_inv, -1.0, 1.0) : 0.0;
    const double skew = skew_alpha_ * q_norm * fv;

    double bid = fv - half_spread - skew;
    double ask = fv + half_spread - skew;

    // Clamp to keep quotes passive (post-only safe): bid must stay below the best
    // ask and ask must stay above the best bid, each by at least one tick.
    if (st.tick_size > 0.0) {
        if (st.last_market_ask > 0.0)
            bid = std::min(bid, st.last_market_ask - st.tick_size);
        if (st.last_market_bid > 0.0)
            ask = std::max(ask, st.last_market_bid + st.tick_size);
    }

    return QuoteTarget{
        .bid = bid,
        .ask = ask,
        .half_spread_bps = half_spread_bps,
    };
}

// ── Requoting ─────────────────────────────────────────────────────────────────

void FairValueMmStrategy::maybe_requote(InstrumentState& st, double net_qty, const QuoteTarget& q, uint64_t ts_ns) {
    if (ts_ns < st.reject_backoff_until_ns)
        return;

    // Drawdown pause.
    if (ts_ns < st.pause_until_ns) {
        if (st.h_bid.live())
            order_mgr_->send_cancel(st.h_bid);
        if (st.h_ask.live())
            order_mgr_->send_cancel(st.h_ask);
        return;
    }

    const double max_inv = effective_max_inventory(st);
    const double q_norm = (max_inv > 0.0) ? std::clamp(net_qty / max_inv, -1.0, 1.0) : 0.0;
    const double ord_qty = effective_order_qty(st);

    // At inventory cap: cancel quotes, send unwind.
    if (std::abs(net_qty) >= max_inv) {
        if (st.h_bid.live())
            order_mgr_->send_cancel(st.h_bid);
        if (st.h_ask.live())
            order_mgr_->send_cancel(st.h_ask);
        if (!st.h_unwind.valid() || st.h_unwind.terminal()) {
            const auto side = (net_qty > 0.0) ? OrderSide::SELL : OrderSide::BUY;
            st.h_unwind = send_unwind_order(st, side, st.last_mid, std::abs(net_qty), kTagUnwindNormal);
        }
        return;
    }

    // Bid side.
    const bool suppress_bid = (q_norm >= one_sided_threshold_);
    if (suppress_bid) {
        if (st.h_bid.live())
            order_mgr_->send_cancel(st.h_bid);
    } else {
        if (st.h_bid.live()) {
            const double move = std::abs(st.last_mid - st.bid_placed_mid) / st.last_mid;
            if (move > requote_threshold_ && !st.h_bid.cancel_pending())
                order_mgr_->send_cancel(st.h_bid);
        } else if (!st.h_bid.cancel_pending()) {
            st.h_bid = send_limit_order(st, OrderSide::BUY, q.bid, ord_qty);
            if (st.h_bid.valid())
                st.bid_placed_mid = st.last_mid;
        }
    }

    // Ask side.
    const bool suppress_ask = (q_norm <= -one_sided_threshold_);
    if (suppress_ask) {
        if (st.h_ask.live())
            order_mgr_->send_cancel(st.h_ask);
    } else {
        if (st.h_ask.live()) {
            const double move = std::abs(st.last_mid - st.ask_placed_mid) / st.last_mid;
            if (move > requote_threshold_ && !st.h_ask.cancel_pending())
                order_mgr_->send_cancel(st.h_ask);
        } else if (!st.h_ask.cancel_pending()) {
            st.h_ask = send_limit_order(st, OrderSide::SELL, q.ask, ord_qty);
            if (st.h_ask.valid())
                st.ask_placed_mid = st.last_mid;
        }
    }
}

// ── Order helpers ─────────────────────────────────────────────────────────────

order::OrderHandle FairValueMmStrategy::send_limit_order(InstrumentState& st,
                                                         OrderSide::Value side,
                                                         double price,
                                                         double qty) {
    qty = bpt::strategy::venue::bump_qty_for_min_notional(qty,
                                                          st.last_mid,
                                                          st.lot_size,
                                                          bpt::strategy::venue::min_notional_usd(st.exchange));

    // Passive rounding: BUY floors (post below), SELL ceils (post above).
    if (st.tick_size > 0.0) {
        if (side == OrderSide::BUY)
            price = std::floor(price / st.tick_size) * st.tick_size;
        else
            price = std::ceil(price / st.tick_size) * st.tick_size;
    }

    const order::NewOrderRequest req{
        .instrument_id = st.instrument_id,
        .exchange_id = st.exchange_id,
        .side = side,
        .type = OrderType::LIMIT,
        .tif = TimeInForce::GTC,
        .price = price,
        .qty = qty,
        .exec_inst = {.post_only = true},
    };

    auto h = order_mgr_->send_new_order(req, kTagQuote);
    if (h.valid()) {
        if (side == OrderSide::BUY)
            st.last_bid_price = price;
        else
            st.last_ask_price = price;
        bpt::common::log::debug(kLog(),
                                "[FVMM:{}] {} LIMIT {:.6f} qty={:.4f} oid={}",
                                correlation_id_,
                                (side == OrderSide::BUY) ? "BUY" : "SELL",
                                price,
                                qty,
                                h.order_id());
    }
    return h;
}

order::OrderHandle FairValueMmStrategy::send_unwind_order(InstrumentState& st,
                                                          OrderSide::Value side,
                                                          double mid,
                                                          double qty,
                                                          uint8_t tag) {
    // Never bump to min_notional: unwind orders are always position-reducing,
    // and bumping beyond the actual position size would flip the sign.
    constexpr double kCrossBps = 20.0;
    const double cross_factor = (side == OrderSide::BUY) ? (1.0 + kCrossBps / 10000.0) : (1.0 - kCrossBps / 10000.0);
    const double price = mid * cross_factor;

    const order::NewOrderRequest req{
        .instrument_id = st.instrument_id,
        .exchange_id = st.exchange_id,
        .side = side,
        .type = OrderType::LIMIT,
        .tif = TimeInForce::IOC,
        .price = price,
        .qty = qty,
        .exec_inst = {},
    };

    auto h = order_mgr_->send_new_order(req, tag);
    if (h.valid()) {
        bpt::common::log::info(kLog(),
                               "[FVMM:{}] unwind {} IOC {:.6f} qty={:.4f} oid={} tag={}",
                               correlation_id_,
                               (side == OrderSide::BUY) ? "BUY" : "SELL",
                               price,
                               qty,
                               h.order_id(),
                               tag);
    }
    return h;
}

double FairValueMmStrategy::effective_order_qty(const InstrumentState& st) const {
    if (order_qty_fraction_ > 0.0 && last_equity_e8_ > 0 && st.last_mid > 0.0) {
        const double equity_usd = static_cast<double>(last_equity_e8_) / 1e8;
        const double qty = order_qty_fraction_ * equity_usd / st.last_mid;
        const double floor_qty = (order_qty_min_ > 0.0) ? order_qty_min_ : st.lot_size;
        return std::max(qty, floor_qty);
    }
    return order_qty_;
}

double FairValueMmStrategy::effective_max_inventory(const InstrumentState& st) const {
    if (max_inventory_fraction_ > 0.0 && last_equity_e8_ > 0 && st.last_mid > 0.0) {
        const double equity_usd = static_cast<double>(last_equity_e8_) / 1e8;
        return max_inventory_fraction_ * equity_usd / st.last_mid;
    }
    return max_inventory_;
}

// ── Execution ─────────────────────────────────────────────────────────────────

void FairValueMmStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    order_mgr_->on_exec_report(rpt);

    const uint64_t instrument_id = rpt.instrumentId();
    const auto it = state_.find(instrument_id);
    if (it == state_.end())
        return;
    auto& st = it->second;

    const auto status = rpt.status();
    const bool is_fill = (status == ExecStatus::FILLED || status == ExecStatus::PARTIAL);
    const bool is_terminal =
        (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED);

    if (is_fill) {
        positions_.on_fill(instrument_id, st.exchange_id, rpt.side(), rpt.filledQty(), rpt.price());
        bpt::common::log::info(kLog(),
                               "[FVMM:{}] fill oid={} side={} qty={:.4f} px={:.6f}",
                               correlation_id_,
                               rpt.orderId(),
                               (rpt.side() == OrderSide::BUY) ? "BUY" : "SELL",
                               static_cast<double>(rpt.filledQty()) / 1e8,
                               static_cast<double>(rpt.price()) / 1e8);
    }

    // Exchange error → exponential-ish backoff (5s × error count, capped).
    if (status == ExecStatus::REJECTED) {
        ++st.consecutive_exchange_errors;
        const uint64_t backoff_ns = 5'000'000'000ULL * std::min(st.consecutive_exchange_errors, 6u);
        st.reject_backoff_until_ns = bpt::strategy::clock::SimClock::now_ns() + backoff_ns;
        bpt::common::log::warn(kLog(),
                               "[FVMM:{}] reject oid={} error#{} backoff={}s",
                               correlation_id_,
                               rpt.orderId(),
                               st.consecutive_exchange_errors,
                               backoff_ns / 1'000'000'000ULL);
    } else {
        st.consecutive_exchange_errors = 0;
    }

    // Identify which of our handles this report belongs to, then clear on terminal.
    const auto h = order_mgr_->find_by_id(rpt.orderId());
    const uint8_t tag = h.valid() ? h.state->tag : kTagQuote;

    if (is_terminal) {
        if (st.h_bid.valid() && st.h_bid.order_id() == rpt.orderId())
            st.h_bid.reset();
        if (st.h_ask.valid() && st.h_ask.order_id() == rpt.orderId())
            st.h_ask.reset();
        if (st.h_unwind.valid() && st.h_unwind.order_id() == rpt.orderId())
            st.h_unwind.reset();
    }

    // Drawdown circuit breaker on quote fills.
    if (is_fill && tag == kTagQuote && pause_below_rpnl_usd_ < 0.0) {
        const auto pos = positions_.get(instrument_id, st.exchange_id);
        if (pos && pos->realized_pnl < pause_below_rpnl_usd_) {
            const uint64_t cooldown_ns = static_cast<uint64_t>(pause_cooldown_s_ * 1e9);
            st.pause_until_ns = bpt::strategy::clock::SimClock::now_ns() + cooldown_ns;
            bpt::common::log::warn(kLog(),
                                   "[FVMM:{}] drawdown rpnl={:.4f} < threshold={:.4f} — pausing {}s",
                                   correlation_id_,
                                   pos->realized_pnl,
                                   pause_below_rpnl_usd_,
                                   pause_cooldown_s_);
        }
    }

    if (shutting_down_ && tag == unwind::GracefulUnwinder::kTag)
        unwinder_.on_exec_report(rpt);
}

// ── Account snapshot ──────────────────────────────────────────────────────────

std::size_t FairValueMmStrategy::on_account_snapshot(bpt::messages::AccountSnapshot& snap) {
    last_snapshot_ns_ = bpt::strategy::clock::SimClock::now_ns();
    last_equity_e8_ = snap.totalEquityE8();

    snap.sbeRewind();
    auto rows = extract_exchange_position_rows(snap);

    // Cache snapshot quantities for shutdown flatten's exchange-authoritative path.
    last_snapshot_qty_e8_.clear();
    for (const auto& [sym, row] : rows)
        last_snapshot_qty_e8_[sym] = row.net_qty_e8;

    // Seed PositionTracker from snapshot (startup position or resync after divergence).
    for (const auto& [id, st] : state_) {
        const auto rit = rows.find(st.symbol);
        if (rit == rows.end())
            continue;
        positions_.seed(id, st.exchange_id, rit->second.net_qty_e8, rit->second.avg_entry_price);
    }

    // Reconcile our tracker against the snapshot.
    std::unordered_map<std::string, int64_t> qty_by_sym;
    std::unordered_map<uint64_t, std::string> id_to_sym;
    for (const auto& [sym, row] : rows)
        qty_by_sym[sym] = row.net_qty_e8;
    for (const auto& [id, st] : state_)
        id_to_sym[id] = st.symbol;

    const ExchangeId::Value eid = state_.empty() ? ExchangeId::NULL_VALUE : state_.begin()->second.exchange_id;
    const auto divs = reconcile(positions_, qty_by_sym, eid, id_to_sym, 100'000'000LL);
    for (const auto& d : divs) {
        bpt::common::log::warn(kLog(),
                               "[FVMM:{}] reconcile div id={} our={:.4f} exch={:.4f}",
                               correlation_id_,
                               d.instrument_id,
                               static_cast<double>(d.our_net_qty_e8) / 1e8,
                               static_cast<double>(d.exchange_net_qty_e8) / 1e8);
    }

    bpt::common::log::info(kLog(),
                           "[FVMM:{}] snapshot equity={:.2f} USD divs={}",
                           correlation_id_,
                           static_cast<double>(last_equity_e8_) / 1e8,
                           divs.size());
    return divs.size();
}

// ── Shutdown flatten ──────────────────────────────────────────────────────────

void FairValueMmStrategy::on_shutdown_flatten() {
    shutting_down_ = true;

    std::vector<unwind::GracefulUnwinder::Instrument> instruments;
    for (auto& [id, st] : state_) {
        order_mgr_->cancel_all(st.exchange_id, id);
        if (st.h_bid.live())
            order_mgr_->send_cancel(st.h_bid);
        if (st.h_ask.live())
            order_mgr_->send_cancel(st.h_ask);

        const double fv = st.fv.last_estimate();
        instruments.push_back({
            .instrument_id = id,
            .exchange_id = st.exchange_id,
            .tick_size = st.tick_size,
            .lot_size = st.lot_size,
            .symbol = st.symbol,
            .price_ref = (!std::isnan(fv) && fv > 0.0) ? fv : st.last_mid,
        });
    }
    unwinder_.arm(std::move(instruments));
}

void FairValueMmStrategy::on_flatten_tick() {
    unwinder_.tick();
}
bool FairValueMmStrategy::has_pending_flatten() const {
    return unwinder_.pending();
}
double FairValueMmStrategy::shutdown_drain_budget_s() const {
    return unwinder_.drain_budget_s();
}

// ── Console state JSON ────────────────────────────────────────────────────────

std::string FairValueMmStrategy::get_strategy_state_json() {
    if (state_.empty())
        return {};

    const auto& [instrument_id, st] = *state_.begin();
    const double net_qty = static_cast<double>(positions_.net_qty(instrument_id, st.exchange_id)) / 1e8;
    const double max_inv = effective_max_inventory(st);
    const auto pos = positions_.get(instrument_id, st.exchange_id);

    std::optional<QuoteTarget> q_opt;
    if (st.ewma_var.count() >= vol_warmup_ticks_ && st.last_mid > 0.0)
        q_opt = compute_quotes(st, net_qty);

    nlohmann::json j;
    j["type"] = "strategyState";
    j["kind"] = "FVMM";
    j["symbol"] = st.symbol;
    j["exchange"] = st.exchange;

    j["mid"] = st.last_mid;
    j["marketBid"] = st.last_market_bid;
    j["marketAsk"] = st.last_market_ask;

    j["sigma2"] = st.ewma_var.value();
    j["sigmaBps"] = std::sqrt(st.ewma_var.value()) * 1e4;
    j["volTicks"] = st.ewma_var.count();
    j["volWarmup"] = vol_warmup_ticks_;
    j["warmedUp"] = st.ewma_var.count() >= vol_warmup_ticks_;

    j["halfSpreadBps"] = q_opt ? q_opt->half_spread_bps : 0.0;
    j["bidPrice"] = st.last_bid_price;
    j["askPrice"] = st.last_ask_price;
    j["bidOrderLive"] = st.h_bid.live();
    j["askOrderLive"] = st.h_ask.live();

    j["inventory"] = net_qty;
    j["maxInventory"] = max_inv;
    j["inventoryPct"] = max_inv > 0.0 ? std::abs(net_qty) / max_inv * 100.0 : 0.0;
    j["realizedPnl"] = pos ? pos->realized_pnl : 0.0;

    j["shuttingDown"] = shutting_down_;
    j["refdataStale"] = refdata_stale_;

    return j.dump();
}

}  // namespace bpt::strategy::strategy

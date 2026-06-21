#include "strategy/strategy/ofi_strategy.h"

#include "strategy/md/subscribe_helpers.h"
#include "strategy/refdata/exchange_id.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/InstrumentType.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>
#include <messages/RejectSource.h>
#include <messages/TimeInForce.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <cmath>

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::TimeInForce;

namespace bpt::strategy::strategy {

namespace {
// Sub-module logger — auto-prefixed with "OFI" via %(logger) in the default
// log pattern. Lazy-initialised because bpt::common::logging::init() runs
// after static initialisation.
quill::Logger* kLog() {
    static quill::Logger* l = bpt::common::logging::get_logger("OFI");
    return l;
}
}  // namespace

static constexpr double kPriceScale = 1e8;
static constexpr double kQtyScale = 1e8;

// Aggressiveness for taker IOC limits — cross the book by this many bps.
// Set to 0: IOC already guarantees immediate match against resting liquidity,
// and any extra cross is pure cost when the fill price reflects the limit.
// The 30-min v2 smoke showed t+1s markout ≈ -1 bp ≈ half_spread + kAggressBps,
// i.e. the "adverse" signal was mostly our own self-inflicted execution cost.
static constexpr double kAggressBps = 0.0;

// ── Constructor ──────────────────────────────────────────────────────────────

OFIStrategy::OFIStrategy(uint64_t correlation_id,
                         const config::StrategyConfig& cfg,
                         refdata::IRefdataClient& refdata,
                         md::IMdClient* md,
                         order::OrderManager* order_mgr)
    : correlation_id_(correlation_id),
      book_levels_(static_cast<int>(cfg.params["book_levels"].value<int64_t>().value_or(5))),
      ofi_window_ns_(static_cast<uint64_t>(cfg.params["ofi_window_ms"].value<double>().value_or(1000.0) * 1e6)),
      entry_threshold_(cfg.params["entry_threshold"].value<double>().value_or(0.35)),
      exit_threshold_(cfg.params["exit_threshold"].value<double>().value_or(0.15)),
      stop_bps_(cfg.params["stop_bps"].value<double>().value_or(8.0)),
      target_bps_(cfg.params["target_bps"].value<double>().value_or(12.0)),
      max_hold_ns_(static_cast<uint64_t>(cfg.params["max_hold_seconds"].value<double>().value_or(30.0) * 1e9)),
      cooldown_ticks_(static_cast<int>(cfg.params["cooldown_ticks"].value<int64_t>().value_or(20))),
      qty_usd_(cfg.params["qty_usd"].value<double>().value_or(200.0)),
      max_spread_bps_(cfg.params["max_spread_bps"].value<double>().value_or(5.0)),
      order_book_depth_(static_cast<uint8_t>(cfg.params["order_book_depth"].value<int64_t>().value_or(5))),
      vol_gate_cfg_{
          cfg.params["vol_gate_max_bps"].value<double>().value_or(0.0),
          static_cast<uint64_t>(cfg.params["vol_gate_window_ms"].value<double>().value_or(1000.0) * 1e6),
          static_cast<uint64_t>(cfg.params["vol_gate_halt_ms"].value<double>().value_or(5000.0) * 1e6),
      },
      instruments_(cfg.instruments),
      md_exchanges_(cfg.md_exchanges),
      venue_exec_(cfg.venue_exec),
      refdata_(refdata),
      md_client_(md),
      order_mgr_(order_mgr) {
    bpt::common::log::info(kLog(),
                           "levels={} window={}ms entry={:.2f} exit={:.2f}",
                           book_levels_,
                           ofi_window_ns_ / 1'000'000,
                           entry_threshold_,
                           exit_threshold_);
    bpt::common::log::info(kLog(),
                           "stop={:.1f}bps target={:.1f}bps max_hold={:.0f}s cooldown={}ticks",
                           stop_bps_,
                           target_bps_,
                           max_hold_ns_ / 1e9,
                           cooldown_ticks_);
    bpt::common::log::info(kLog(),
                           "qty_usd={:.0f} max_spread={:.1f}bps depth={}",
                           qty_usd_,
                           max_spread_bps_,
                           order_book_depth_);
    if (vol_gate_cfg_.max_bps_per_window > 0.0) {
        bpt::common::log::info(kLog(),
                               "vol_gate max_bps={:.1f} window={}ms halt={}ms",
                               vol_gate_cfg_.max_bps_per_window,
                               vol_gate_cfg_.window_ns / 1'000'000,
                               vol_gate_cfg_.halt_duration_ns / 1'000'000);
    } else {
        bpt::common::log::info(kLog(), "vol_gate disabled (vol_gate_max_bps=0)");
    }
}

// ── IStrategy lifecycle ──────────────────────────────────────────────────────

void OFIStrategy::start() {
    refdata_.subscribe(correlation_id_, CanonicalResolver::build_filters(instruments_, md_exchanges_));
}

void OFIStrategy::on_snapshot(const refdata::InstrumentCache& cache) {
    if (!state_.empty()) {
        bpt::common::log::debug(kLog(), "Ignoring duplicate snapshot ({} instruments)", cache.size());
        return;
    }

    const OFICalculator::Config ofi_cfg{book_levels_, ofi_window_ns_};

    for (const auto& r : CanonicalResolver::resolve_instruments(cache, instruments_, md_exchanges_)) {
        InstrumentState st(ofi_cfg, vol_gate_cfg_);
        st.instrument_id = r.instrument_id;
        st.symbol = r.instrument.symbol;
        st.exchange = r.instrument.exchange;
        st.exchange_id = r.exchange_id;
        st.tick_size = r.instrument.tick_size;
        st.lot_size = r.instrument.lot_size;

        bpt::common::log::info(kLog(),
                               "Instrument [{}] {} @ {} tick={} lot={}",
                               r.instrument_id,
                               r.instrument.symbol,
                               r.instrument.exchange,
                               r.instrument.tick_size,
                               r.instrument.lot_size);
        state_.emplace(r.instrument_id, std::move(st));
    }
    bpt::common::log::info(kLog(), "Resolved {} instrument(s)", state_.size());

    if (md_client_)
        md_client_->subscribe(correlation_id_, md::build_subscriptions(state_, order_book_depth_));
}

void OFIStrategy::on_delta(const refdata::Instrument& /*inst*/, bpt::messages::DeltaUpdateType::Value /*type*/) {}

void OFIStrategy::on_trade(const bpt::messages::MdTrade& /*tick*/) {}

// ── Market data ──────────────────────────────────────────────────────────────

void OFIStrategy::on_bbo(const bpt::messages::MdMarketData& tick) {
    auto it = state_.find(tick.instrumentId());
    if (it == state_.end())
        return;

    InstrumentState& st = it->second;
    const double bid = tick.bidPrice();
    const double ask = tick.askPrice();
    if (bid <= 0.0 || ask <= 0.0 || ask <= bid)
        return;

    st.bid = bid;
    st.ask = ask;
    st.last_bbo_ns = tick.timestampNs();

    // Feed the vol gate with every mid update so the rolling window is
    // maintained regardless of whether we're checking halt status right
    // now. update_and_check logs a warning on trip so operators can see
    // it in the log.
    const double mid = (bid + ask) * 0.5;
    const bool was_halted = st.vol_gate.is_halted(st.last_bbo_ns);
    const bool now_halted = st.vol_gate.update_and_check(mid, st.last_bbo_ns);
    if (now_halted && !was_halted) {
        bpt::common::log::warn(kLog(),
                               "{} VOL HALT tripped last_trip={:.1f}bps — pausing entries for {}ms",
                               st.symbol,
                               st.vol_gate.last_trip_bps(),
                               vol_gate_cfg_.halt_duration_ns / 1'000'000);
    } else if (was_halted && !now_halted) {
        bpt::common::log::info(kLog(), "{} vol halt cleared — entries re-enabled", st.symbol);
    }

    // Walk pending mark-outs with the freshly updated mid.
    check_markouts(st, st.last_bbo_ns);

    // Time-based exit: check even on BBO ticks so positions don't overrun
    // max_hold while waiting for the next book update.
    if (st.pos != Position::FLAT && max_hold_ns_ > 0 && st.last_bbo_ns - st.entry_ns > max_hold_ns_) {
        bpt::common::log::info(kLog(),
                               "{} time_stop ({}s) — exiting {}",
                               st.symbol,
                               (st.last_bbo_ns - st.entry_ns) / 1'000'000'000ULL,
                               st.pos == Position::LONG ? "LONG" : "SHORT");
        const auto exit_side = (st.pos == Position::LONG) ? OrderSide::SELL : OrderSide::BUY;
        st.active_is_entry = false;
        fire_order(st, exit_side, qty_usd_);
        st.pos = Position::FLAT;
        st.cooldown_ticks_remaining = cooldown_ticks_;
    }
}

void OFIStrategy::on_order_book(const bpt::messages::MdOrderBook& book) {
    auto it = state_.find(book.instrumentId());
    if (it == state_.end())
        return;
    InstrumentState& st = it->second;

    // SBE group iterators share the parent position — bids must be fully
    // consumed before calling asks().
    auto& mutable_book = const_cast<bpt::messages::MdOrderBook&>(book);

    std::vector<OFICalculator::Level> bids;
    std::vector<OFICalculator::Level> asks;
    bids.reserve(static_cast<size_t>(book_levels_));
    asks.reserve(static_cast<size_t>(book_levels_));

    // MdOrderBook.qty is a native SBE double (not fixed-point) — see
    // MdOrderBook.h::qty(). order_book_state.cpp treats it as such;
    // this path was double-scaling by 1e8 so OFI values came out off
    // by 8 orders of magnitude. Caught before OFI went live.
    auto& bids_grp = mutable_book.bids();
    while (bids_grp.hasNext()) {
        auto& lvl = bids_grp.next();
        bids.emplace_back(lvl.price(), lvl.qty());
    }
    auto& asks_grp = mutable_book.asks();
    while (asks_grp.hasNext()) {
        auto& lvl = asks_grp.next();
        asks.emplace_back(lvl.price(), lvl.qty());
    }

    const uint64_t now_ns = book.timestampNs();
    const double ofi = st.ofi.update(bids, asks, now_ns);

    if (st.cooldown_ticks_remaining > 0)
        --st.cooldown_ticks_remaining;

    // Bail on any outstanding order — one in flight at a time.
    if (st.active_order_id != 0)
        return;

    if (st.pos == Position::FLAT) {
        try_enter(st, ofi, now_ns);
    } else {
        try_exit(st, ofi, now_ns);
    }
}

// ── Entry ───────────────────────────────────────────────────────────────────

void OFIStrategy::try_enter(InstrumentState& st, double ofi_value, uint64_t now_ns) {
    if (st.cooldown_ticks_remaining > 0)
        return;
    if (!st.ofi.is_warm())
        return;
    if (st.bid <= 0.0 || st.ask <= 0.0)
        return;
    if (st.vol_gate.is_halted(now_ns))
        return;

    const double mid = (st.bid + st.ask) * 0.5;
    const double spread_bps = (st.ask - st.bid) / mid * 1e4;
    if (spread_bps > max_spread_bps_)
        return;

    if (ofi_value > entry_threshold_) {
        bpt::common::log::info(kLog(),
                               "{} ENTER LONG ofi={:.3f} mid={:.4f} spread={:.1f}bps",
                               st.symbol,
                               ofi_value,
                               mid,
                               spread_bps);
        st.active_is_entry = true;
        fire_order(st, OrderSide::BUY, qty_usd_);
        st.pos = Position::LONG;
        st.entry_price = mid;
        st.entry_ns = now_ns;
    } else if (ofi_value < -entry_threshold_) {
        bpt::common::log::info(kLog(),
                               "{} ENTER SHORT ofi={:.3f} mid={:.4f} spread={:.1f}bps",
                               st.symbol,
                               ofi_value,
                               mid,
                               spread_bps);
        st.active_is_entry = true;
        fire_order(st, OrderSide::SELL, qty_usd_);
        st.pos = Position::SHORT;
        st.entry_price = mid;
        st.entry_ns = now_ns;
    }
}

// ── Exit ────────────────────────────────────────────────────────────────────

void OFIStrategy::try_exit(InstrumentState& st, double ofi_value, uint64_t now_ns) {
    if (st.bid <= 0.0 || st.ask <= 0.0)
        return;
    const double mid = (st.bid + st.ask) * 0.5;

    const double move_bps = (mid - st.entry_price) / st.entry_price * 1e4;
    const bool is_long = (st.pos == Position::LONG);
    const double pnl_bps = is_long ? move_bps : -move_bps;

    const char* reason = nullptr;
    if (target_bps_ > 0.0 && pnl_bps >= target_bps_)
        reason = "target";
    else if (stop_bps_ > 0.0 && pnl_bps <= -stop_bps_)
        reason = "stop";
    else if (max_hold_ns_ > 0 && now_ns - st.entry_ns > max_hold_ns_)
        reason = "time_stop";
    else if (is_long && ofi_value < -exit_threshold_)
        reason = "signal_flip";
    else if (!is_long && ofi_value > exit_threshold_)
        reason = "signal_flip";

    if (!reason)
        return;

    bpt::common::log::info(kLog(),
                           "{} EXIT {} reason={} pnl={:.1f}bps ofi={:.3f}",
                           st.symbol,
                           is_long ? "LONG" : "SHORT",
                           reason,
                           pnl_bps,
                           ofi_value);

    const auto exit_side = is_long ? OrderSide::SELL : OrderSide::BUY;
    st.active_is_entry = false;
    fire_order(st, exit_side, qty_usd_);
    st.pos = Position::FLAT;
    st.cooldown_ticks_remaining = cooldown_ticks_;
}

// ── Order submission ────────────────────────────────────────────────────────

void OFIStrategy::fire_order(InstrumentState& st, bpt::messages::OrderSide::Value side, double qty_usd) {
    if (!order_mgr_) {
        bpt::common::log::warn(kLog(), "{} order_mgr null — dropping order", st.symbol);
        return;
    }

    const double mid = (st.bid + st.ask) * 0.5;
    if (mid <= 0.0)
        return;

    const double qty = qty_usd / mid;

    // Aggressive LIMIT IOC — cross the spread so it takes immediately.
    const double cross = mid * (kAggressBps / 1e4);
    const double price = (side == OrderSide::BUY) ? (st.ask + cross) : (st.bid - cross);

    const uint64_t oid = order_mgr_
                             ->send_new_order(order::NewOrderRequest{
                                 .instrument_id = st.instrument_id,
                                 .exchange_id = st.exchange_id,
                                 .side = side,
                                 .type = OrderType::LIMIT,
                                 .tif = TimeInForce::IOC,
                                 .price = price,
                                 .qty = qty,
                             })
                             .order_id();
    if (oid == 0) {
        bpt::common::log::warn(kLog(), "{} place_order rejected — preflight failed", st.symbol);
        return;
    }
    st.active_order_id = oid;
    order_to_instrument_[oid] = st.instrument_id;
}

// ── Mark-out diagnostic ─────────────────────────────────────────────────────

void OFIStrategy::check_markouts(InstrumentState& st, uint64_t now_ns) {
    if (st.pending_markouts.empty())
        return;
    if (st.bid <= 0.0 || st.ask <= 0.0)
        return;
    const double mid = (st.bid + st.ask) * 0.5;

    constexpr uint64_t k1s = 1'000'000'000ULL;
    constexpr uint64_t k5s = 5ULL * 1'000'000'000ULL;
    constexpr uint64_t k30s = 30ULL * 1'000'000'000ULL;

    auto log_markout = [&](const char* label, const MarkOut& mo) {
        const double move = (mid - mo.fill_price) * static_cast<double>(mo.side_sign);
        const double bps = (mo.fill_price > 0.0) ? (move / mo.fill_price * 1e4) : 0.0;
        bpt::common::log::info("[OFI markout] {} order_id={} kind={} side={} fill={:.4f} mid={:.4f} {}={:+.2f}bps",
                               st.symbol,
                               mo.order_id,
                               mo.is_entry ? "ENTRY" : "EXIT",
                               mo.side_sign > 0 ? "LONG" : "SHORT",
                               mo.fill_price,
                               mid,
                               label,
                               bps);
    };

    for (auto& mo : st.pending_markouts) {
        const uint64_t age = (now_ns > mo.fill_ns) ? (now_ns - mo.fill_ns) : 0;
        if (!mo.logged_1s && age >= k1s) {
            log_markout("t+1s", mo);
            mo.logged_1s = true;
        }
        if (!mo.logged_5s && age >= k5s) {
            log_markout("t+5s", mo);
            mo.logged_5s = true;
        }
        if (!mo.logged_30s && age >= k30s) {
            log_markout("t+30s", mo);
            mo.logged_30s = true;
        }
    }

    // Evict fully-logged entries from the front. Anchors are monotonic
    // so once 30s is logged the entry is done.
    while (!st.pending_markouts.empty() && st.pending_markouts.front().logged_30s)
        st.pending_markouts.pop_front();
}

// ── Execution reports ───────────────────────────────────────────────────────

void OFIStrategy::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const uint64_t order_id = rpt.orderId();
    auto inst_it = order_to_instrument_.find(order_id);
    if (inst_it == order_to_instrument_.end())
        return;

    auto st_it = state_.find(inst_it->second);
    if (st_it == state_.end())
        return;
    InstrumentState& st = st_it->second;

    const auto status = rpt.status();
    if (status == ExecStatus::ACKED)
        return;

    if (status == ExecStatus::REJECTED) {
        bpt::common::log::warn(kLog(),
                               "{} order_id={} REJECTED reason={} source={}",
                               st.symbol,
                               order_id,
                               bpt::messages::RejectReason::c_str(rpt.rejectReason()),
                               bpt::messages::RejectSource::c_str(rpt.rejectSource()));
    } else {
        bpt::common::log::info(kLog(),
                               "{} order_id={} {} filled={:.6f}@{:.4f}",
                               st.symbol,
                               order_id,
                               bpt::messages::ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / kQtyScale,
                               static_cast<double>(rpt.price()) / kPriceScale);
    }

    // Mark-out diagnostic: on the FINAL fill (status=FILLED) of each
    // order, record the fill price + time + signed direction. on_bbo
    // walks the deque and emits mid-mark-out logs at 1s / 5s / 30s
    // anchors. We deliberately skip PARTIAL so IOC sweeps hitting
    // multiple levels produce one mark-out per order, not one per
    // level. Cap the deque at a dozen so a pathological fill burst
    // can't grow it unbounded.
    if (status == ExecStatus::FILLED) {
        if (st.pending_markouts.size() < 12) {
            MarkOut mo;
            mo.fill_price = static_cast<double>(rpt.price()) / kPriceScale;
            mo.fill_ns = st.last_bbo_ns;
            mo.side_sign = (rpt.side() == bpt::messages::OrderSide::BUY) ? +1 : -1;
            mo.order_id = order_id;
            mo.is_entry = st.active_is_entry;
            st.pending_markouts.push_back(mo);
        }
    }

    const bool is_terminal =
        (status == ExecStatus::FILLED || status == ExecStatus::CANCELLED || status == ExecStatus::REJECTED);
    if (!is_terminal)
        return;

    // IOC order didn't fill — treat the attempted leg as cancelled.
    // If this was an entry, roll back to FLAT; cooldown to avoid chase.
    // If this was an exit, the position is still on — clear active_order_id
    // so the next tick can retry.
    if (status != ExecStatus::FILLED && order_id == st.active_order_id) {
        bpt::common::log::info(kLog(), "{} order_id={} did not fill — reverting state", st.symbol, order_id);
        st.pos = Position::FLAT;
        st.cooldown_ticks_remaining = cooldown_ticks_;
    }

    if (order_id == st.active_order_id)
        st.active_order_id = 0;
    order_to_instrument_.erase(order_id);
}

// ── Shutdown flatten ────────────────────────────────────────────────────────

void OFIStrategy::on_shutdown_flatten() {
    int flattened = 0;
    for (auto& [id, st] : state_) {
        if (st.pos == Position::FLAT)
            continue;
        const auto exit_side = (st.pos == Position::LONG) ? OrderSide::SELL : OrderSide::BUY;
        bpt::common::log::warn(kLog(),
                               "SHUTDOWN FLATTEN {} closing {} via IOC",
                               st.symbol,
                               st.pos == Position::LONG ? "LONG" : "SHORT");
        st.active_is_entry = false;
        fire_order(st, exit_side, qty_usd_);
        st.pos = Position::FLAT;
        ++flattened;
    }
    if (flattened > 0)
        bpt::common::log::warn(kLog(), "shutdown flatten fired {} closing order(s)", flattened);
}

}  // namespace bpt::strategy::strategy

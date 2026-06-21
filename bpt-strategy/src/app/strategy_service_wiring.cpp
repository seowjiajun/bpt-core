#include "strategy/app/strategy_service.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/RejectReason.h>

#include <bpt_common/util/tsc_clock.h>
#include <chrono>
#include <filesystem>

// CRTP Handler implementations. The bus's templated clients dispatch
// directly into these member functions — no `std::function` indirection,
// no vtable on the per-frame path.
//
// The wire_*_callbacks() functions just install StrategyService as the
// Handler on each client; the heavy lifting now lives in the on_* member
// functions below.

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::RefDataErrorType;
using bpt::messages::RejectReason;
using namespace std::chrono_literals;

namespace bpt::strategy {

// ── Handler wiring (set_handler installs `this` once at startup) ──────

void StrategyService::wire_refdata_callbacks() {
    if (bus_.refdata)
        bus_.refdata->set_handler(this);
}

void StrategyService::wire_md_callbacks() {
    if (bus_.md)
        bus_.md->set_handler(this);
}

void StrategyService::wire_vol_callbacks() {
    if (bus_.vol)
        bus_.vol->set_handler(this);
}

void StrategyService::wire_order_callbacks() {
    if (order_mgr_) {
        order_mgr_->on_order_placed = [this](uint64_t /*order_id*/) {
            // curr_tick_ts_ns_ is WallClock-sourced (stamped by bpt-md-gateway and
            // re-read via tick.timestampNs()), so this side must match.
            const uint64_t now = bpt::common::util::WallClock::now_ns();
            if (now <= curr_tick_ts_ns_)
                return;
            const uint64_t delta = now - curr_tick_ts_ns_;
            order_lat_hist_.record(delta);
            metrics_.tick_to_order_ns_hist->Observe(static_cast<double>(delta));
        };
    }
    if (bus_.order_gw)
        bus_.order_gw->set_handler(this);
}

// ── MdClient Handler methods ──────────────────────────────────────────

void StrategyService::on_md_service_heartbeat() noexcept {
    last_md_hb_recv_ns_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void StrategyService::on_bbo(const bpt::messages::MdMarketData& tick) noexcept {
    static uint64_t bbo_count = 0;
    if (++bbo_count <= 10 || bbo_count % 1000 == 0) {
        bpt::common::log::info("BBO tick #{}: id={} bid={:.4f} ask={:.4f}",
                               bbo_count,
                               tick.instrumentId(),
                               tick.bidPrice(),
                               tick.askPrice());
    }
    metrics_.md_ticks_total->Increment();
    if (trading_paused_ || trading_halted_)
        return;

    // Cross-service timestamp comparison: bpt-md-gateway stamps tick.timestampNs()
    // with WallClock (CLOCK_REALTIME), so we read the same clock here.
    // Using TscClock would inherit per-process calibration drift and
    // silently underflow the delta when strategy calibration is behind
    // bpt-md-gateway's.
    curr_tick_ts_ns_ = tick.timestampNs();

    strategy_->on_bbo(tick);
    const uint64_t t3 = bpt::common::util::WallClock::now_ns();
    if (t3 > curr_tick_ts_ns_) {
        const uint64_t delta = t3 - curr_tick_ts_ns_;
        if (bbo_count <= 3)
            bpt::common::log::debug("[Latency] bbo raw delta={}ns", delta);
        tick_lat_hist_.record(delta);
        metrics_.tick_to_strategy_ns_hist->Observe(static_cast<double>(delta));
    }
}

void StrategyService::on_trade(const bpt::messages::MdTrade& tick) noexcept {
    metrics_.md_ticks_total->Increment();
    if (trading_paused_ || trading_halted_)
        return;

    curr_tick_ts_ns_ = tick.timestampNs();

    strategy_->on_trade(tick);
    const uint64_t t3 = bpt::common::util::WallClock::now_ns();
    if (t3 > curr_tick_ts_ns_) {
        const uint64_t delta = t3 - curr_tick_ts_ns_;
        tick_lat_hist_.record(delta);
        metrics_.tick_to_strategy_ns_hist->Observe(static_cast<double>(delta));
    }
}

void StrategyService::on_order_book(const bpt::messages::MdOrderBook& book) noexcept {
    if (trading_paused_ || trading_halted_)
        return;
    curr_tick_ts_ns_ = book.timestampNs();
    strategy_->on_order_book(book);
    const uint64_t t3 = bpt::common::util::WallClock::now_ns();
    if (t3 > curr_tick_ts_ns_) {
        const uint64_t delta = t3 - curr_tick_ts_ns_;
        tick_lat_hist_.record(delta);
        metrics_.tick_to_strategy_ns_hist->Observe(static_cast<double>(delta));
    }
}

// ── RefdataClient Handler methods ─────────────────────────────────────

void StrategyService::on_refdata_ready(uint8_t exchanges_loaded,
                                       uint16_t instrument_count,
                                       bool fee_schedules_loaded,
                                       bool funding_rates_loaded) {
    startup_gate_->on_refdata_ready(exchanges_loaded, instrument_count, fee_schedules_loaded, funding_rates_loaded);
}

void StrategyService::on_refdata_error(bpt::messages::RefDataErrorType::Value error_type,
                                       bpt::messages::ExchangeId::Value exchange_id,
                                       uint64_t instrument_id) {
    bpt::common::log::error("RefDataError: type={} exchange={} instrument={}",
                            RefDataErrorType::c_str(error_type),
                            ExchangeId::c_str(exchange_id),
                            instrument_id);
}

void StrategyService::on_refdata_snapshot_complete(const refdata::InstrumentCache& cache) {
    strategy_->on_snapshot(cache);

    // Warm-start load: instruments are resolved and state_ entries
    // exist, so saved EWMA / regime state has somewhere to land.
    // Empty state_dir disables the feature (default).
    const auto& ws = cfg_.strat.strategy.warm_start;
    if (!ws.state_dir.empty()) {
        const auto path = std::filesystem::path(ws.state_dir) / (std::to_string(cfg_.strat.correlation_id) + ".json");
        strategy_->load_state(path.string(), ws.max_age_s);
    }

    startup_gate_->on_refdata_snapshot_complete();
}

void StrategyService::on_refdata_delta(const refdata::Instrument& inst,
                                       bpt::messages::DeltaUpdateType::Value update_type) {
    strategy_->on_delta(inst, update_type);
}

void StrategyService::on_refdata_gap_detected() {
    // Today nothing — refdata's own cache reset + re-subscribe path runs
    // on the client side; strategy is informed but doesn't need to act
    // beyond that. Leave as a hook for future resubscribe orchestration.
}

// ── VolSurfaceClient Handler methods ──────────────────────────────────

void StrategyService::on_vol_surface(bpt::messages::VolSurface& surface) {
    bpt::common::log::info("VolSurface received: exchange={} underlying={}",
                           ExchangeId::c_str(surface.exchangeId()),
                           surface.getUnderlyingAsString());
    strategy_->on_vol_surface(surface);
}

void StrategyService::on_pricer_ready(uint8_t exchanges_loaded, uint16_t underlying_count, uint32_t point_count) {
    bpt::common::log::info("PricerReady: exchanges=0x{:02x} underlyings={} points={}",
                           exchanges_loaded,
                           underlying_count,
                           point_count);
    pricer_ready_ = true;
}

// ── OrderGatewayClient Handler methods ────────────────────────────────

void StrategyService::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    const auto status = rpt.status();
    const double price_d = static_cast<double>(rpt.price()) / 1e8;
    if (status == ExecStatus::ACKED) {
        bpt::common::log::debug("ExecReport order_id={} ACKED price={:.2f}", rpt.orderId(), price_d);
    } else if (status == ExecStatus::REJECTED) {
        bpt::common::log::info("ExecReport order_id={} REJECTED reason={} price={:.2f}",
                               rpt.orderId(),
                               RejectReason::c_str(rpt.rejectReason()),
                               price_d);
    } else {
        bpt::common::log::info("ExecReport order_id={} status={} filled_qty={:.6f} price={:.2f}",
                               rpt.orderId(),
                               ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / 1e8,
                               price_d);
    }
    metrics_.exec_reports_total->Increment();
    strategy_->on_exec_report(rpt);
}

void StrategyService::on_og_heartbeat(const bpt::messages::OrderGatewayHeartbeat& /*hb*/) {
    last_gw_hb_recv_ns_ = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void StrategyService::on_account_snapshot(bpt::messages::AccountSnapshot& snap) {
    const auto exchange_id = snap.exchangeId();
    bpt::common::log::info("AccountSnapshot received: exchange={} balance={:.2f} positions={}",
                           ExchangeId::c_str(exchange_id),
                           static_cast<double>(snap.availableBalanceE8()) / 1e8,
                           snap.positions().count());

    // Stamp arrival time in wall ns so Alertmanager can fire on
    // staleness (time() - gauge/1e9 > threshold). Wall (CLOCK_REALTIME)
    // not monotonic — Prometheus aligns this with scrape-time clocks.
    const uint64_t recv_ns = bpt::common::util::WallClock::now_ns();
    metrics_.account_snapshot_last_recv_ns(ExchangeId::c_str(exchange_id)).Set(static_cast<double>(recv_ns));

    startup_gate_->on_account_snapshot(exchange_id);
    const std::size_t divergences = strategy_->on_account_snapshot(snap);
    if (divergences > 0) {
        // One counter tick per reconcile that produced any drift.
        // The individual (instrument_id, symbol, diff) details are
        // already in the WARN logs; the counter is the coarse
        // "something's been drifting today" surface for alerting.
        metrics_.reconciliation_divergences_total->Increment();
    }
}

// ── ToxicitySubscriber Handler ────────────────────────────────────────

void StrategyService::on_toxicity_update(const bpt::analytics::messaging::ToxicityUpdate& u) {
    strategy_->on_toxicity_update(u);
}

// ── ConsoleControlSubscriber Handler ──────────────────────────────────

void StrategyService::on_console_command(uint8_t cmd) {
    if (cmd == 0x00 && !trading_halted_) {
        trading_halted_ = true;
        metrics_.trading_halted->Set(1.0);
        bpt::common::log::warn("TRADING HALTED via console kill-switch");
    } else if (cmd == 0x01 && trading_halted_) {
        trading_halted_ = false;
        metrics_.trading_halted->Set(0.0);
        bpt::common::log::info("Trading RESUMED via console");
    }
}

}  // namespace bpt::strategy

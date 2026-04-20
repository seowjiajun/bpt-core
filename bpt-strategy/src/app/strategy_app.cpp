#include "strategy/app/strategy_app.h"

#include "strategy/order/aeron_order_gateway_client.h"
#include "strategy/order/paper_order_gateway_client.h"
#include "strategy/strategy/strategy_factory.h"

#include <analytics/messaging/toxicity_update.h>

#include <messages/BacktestCommand.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/RejectReason.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/signal.h>

using namespace std::chrono_literals;
using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::RefDataErrorType;
using bpt::messages::RejectReason;

namespace bpt::strategy {

StrategyApp::StrategyApp(config::AppConfig cfg, std::shared_ptr<aeron::Aeron> aeron)
    : cfg_(std::move(cfg)),
      aeron_(aeron),
      metrics_(cfg_.base.metrics_port),
      fee_cache_(cfg_.strat.strategy.schedule.max_refdata_staleness_ns),
      funding_rate_cache_(cfg_.strat.strategy.schedule.max_refdata_staleness_ns) {
    const auto& ac = cfg_.aeron;
    const auto& fc = cfg_.strat;

    refdata_ = std::make_unique<refdata::RefdataClient>(aeron,
                                                        ac.refdata_control.channel,
                                                        ac.refdata_control.stream_id,
                                                        ac.refdata_snapshot.stream_id,
                                                        ac.refdata_delta.stream_id,
                                                        ac.fee_schedule.stream_id,
                                                        ac.funding_rate.stream_id,
                                                        ac.refdata_status.stream_id,
                                                        fee_cache_,
                                                        funding_rate_cache_);

    if (ac.md_control.stream_id != 0) {
        md_client_ = std::make_unique<md::MdClient>(aeron,
                                                    ac.md_control.channel,
                                                    ac.md_control.stream_id,
                                                    ac.md_data.stream_id,
                                                    ac.md_ack_hb.stream_id);
    }

    if (ac.order.stream_id != 0) {
        if (cfg_.strat.strategy.paper_mode) {
            // Canary / shadow run: swallow orders locally, synthesise
            // fills from the MD stream. Exchange never sees anything.
            auto paper = std::make_unique<order::PaperOrderGatewayClient>();
            paper_gw_ = paper.get();
            order_gw_ = std::move(paper);
            bpt::common::log::warn("================================================");
            bpt::common::log::warn(" PAPER MODE  —  orders will NOT reach the exchange ");
            bpt::common::log::warn("================================================");
        } else {
            order_gw_ = std::make_unique<order::AeronOrderGatewayClient>(aeron,
                                                                    ac.order.channel,
                                                                    ac.order.stream_id,
                                                                    ac.exec_report.stream_id,
                                                                    ac.heartbeat.stream_id,
                                                                    ac.account_snapshot.stream_id);
        }
    }

    if (ac.vol_surface.stream_id != 0) {
        vol_client_ = std::make_unique<vol::VolSurfaceClient>(aeron,
                                                              ac.vol_surface.channel,
                                                              ac.vol_surface.stream_id,
                                                              ac.pricer_status.stream_id);
        bpt::common::log::info("VolSurfaceClient ready: surface={} status={}",
                       ac.vol_surface.stream_id,
                       ac.pricer_status.stream_id);
    }

    if (ac.toxicity.stream_id != 0) {
        tyr_sub_ = bpt::common::aeron::wait_for_subscription(aeron, ac.toxicity.channel, ac.toxicity.stream_id);
        bpt::common::log::info("Analytics toxicity subscription ready: {} stream {}",
                       ac.toxicity.channel, ac.toxicity.stream_id);
    }

    if (order_gw_) {
        order_mgr_ = std::make_unique<order::OrderManager>(*order_gw_, refdata_->cache());
    }

    strategy_ = strategy::StrategyFactory::create(fc, *refdata_, md_client_.get(), order_mgr_.get(), vol_client_.get());

    startup_gate_ = std::make_unique<app::StartupGate>(*refdata_,
                                                       order_gw_.get(),
                                                       *strategy_,
                                                       metrics_,
                                                       fc.strategy.schedule.configured_exchanges_mask,
                                                       fc.correlation_id);

    wire_refdata_callbacks();
    wire_md_callbacks();
    wire_vol_callbacks();
    wire_order_callbacks();

    if (cfg_.backtest_mode) {
        backtest_client_ =
            std::make_unique<backtest::BacktestClient>(aeron,
                                                       ac.backtest_control.channel,
                                                       ac.backtest_control.stream_id,  // sub: Backtester → Strategy
                                                       ac.backtest_ack.stream_id);     // pub: Strategy → Backtester
        bpt::common::log::info("Backtest mode enabled: ctrl_sub={} ack_pub={}",
                       ac.backtest_control.stream_id,
                       ac.backtest_ack.stream_id);
    }

    // Dashboard control subscription — halt/resume from the bridge.
    // 1-byte messages: 0x00 = HALT, 0x01 = RESUME.  Only enabled in
    // live/paper mode (not backtest — backtest has its own control channel).
    if (!cfg_.backtest_mode && ac.dashboard_control.stream_id != 0) {
        const int64_t reg_id = aeron->addSubscription(
            ac.dashboard_control.channel, ac.dashboard_control.stream_id);
        for (int i = 0; i < 500; ++i) {
            dashboard_ctrl_sub_ = aeron->findSubscription(reg_id);
            if (dashboard_ctrl_sub_) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (dashboard_ctrl_sub_) {
            bpt::common::log::info("Dashboard control subscription ready on stream {}",
                           ac.dashboard_control.stream_id);
        } else {
            bpt::common::log::warn("Dashboard control subscription unavailable");
        }
    }

    // Portfolio snapshot publication — JSON-serialized strategy state for
    // the dashboard's options panels, throttled to ~10 Hz inside the
    // publisher. Disabled in backtest mode.
    if (!cfg_.backtest_mode) {
        portfolio_snap_pub_ = std::make_unique<dashboard::PortfolioSnapshotPublisher>(
            aeron, ac.dashboard_snapshot.channel, ac.dashboard_snapshot.stream_id);
    }
}

void StrategyApp::wire_refdata_callbacks() {
    refdata_->on_ready = [this](uint8_t exchanges_loaded,
                                uint16_t instrument_count,
                                bool fee_schedules_loaded,
                                bool funding_rates_loaded) {
        startup_gate_->on_refdata_ready(exchanges_loaded, instrument_count,
                                        fee_schedules_loaded, funding_rates_loaded);
    };

    refdata_->on_error = [](RefDataErrorType::Value error_type, ExchangeId::Value exchange_id, uint64_t instrument_id) {
        bpt::common::log::error("RefDataError: type={} exchange={} instrument={}",
                        RefDataErrorType::c_str(error_type),
                        ExchangeId::c_str(exchange_id),
                        instrument_id);
    };

    refdata_->on_snapshot_complete = [this](const refdata::InstrumentCache& cache) {
        strategy_->on_snapshot(cache);

        // Warm-start load: instruments are resolved and state_ entries
        // exist, so saved EWMA / regime state has somewhere to land.
        // Empty state_dir disables the feature (default).
        const auto& ws = cfg_.strat.strategy.warm_start;
        if (!ws.state_dir.empty()) {
            const auto path = std::filesystem::path(ws.state_dir) /
                              (std::to_string(cfg_.strat.correlation_id) + ".json");
            strategy_->load_state(path.string(), ws.max_age_s);
        }

        startup_gate_->on_refdata_snapshot_complete();
    };

    refdata_->on_delta = [this](const refdata::Instrument& inst,
                                bpt::messages::DeltaUpdateType::Value update_type) {
        strategy_->on_delta(inst, update_type);
    };
}

void StrategyApp::wire_md_callbacks() {
    if (!md_client_) return;

    md_client_->on_service_heartbeat = [this]() {
        last_md_hb_recv_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    };

    md_client_->on_bbo = [this](const bpt::messages::MdMarketData& tick) {
        static uint64_t bbo_count = 0;
        if (++bbo_count <= 10 || bbo_count % 1000 == 0) {
            bpt::common::log::info("BBO tick #{}: id={} bid={:.4f} ask={:.4f}",
                           bbo_count, tick.instrumentId(), tick.bidPrice(), tick.askPrice());
        }
        metrics_.md_ticks_total->Increment();
        if (trading_paused_ || trading_halted_) return;

        // Cross-service timestamp comparison: bpt-md-gateway stamps tick.timestampNs()
        // with WallClock (CLOCK_REALTIME), so we read the same clock here.
        // Using TscClock would inherit per-process calibration drift and
        // silently underflow the delta when strategy calibration is behind
        // bpt-md-gateway's.
        curr_tick_ts_ns_ = tick.timestampNs();

        // Paper mode: feed the fill engine BEFORE the strategy so
        // any IOC submit in the strategy callback sees a fresh BBO
        // and any fills triggered by this tick arrive on the next
        // order_gw_ poll — mirrors the exchange match-then-publish
        // ordering real trading observes downstream.
        if (paper_gw_) {
            paper_gw_->feed_bbo(tick.instrumentId(),
                                tick.bidPrice(),
                                tick.askPrice(),
                                tick.timestampNs());
        }

        strategy_->on_bbo(tick);
        const uint64_t t3 = bpt::common::util::WallClock::now_ns();
        if (t3 > curr_tick_ts_ns_) {
            const uint64_t delta = t3 - curr_tick_ts_ns_;
            if (bbo_count <= 3)
                bpt::common::log::debug("[Latency] bbo raw delta={}ns", delta);
            tick_lat_hist_.record(delta);
            metrics_.tick_to_strategy_ns_hist->Observe(static_cast<double>(delta));
        }
    };

    md_client_->on_trade = [this](const bpt::messages::MdTrade& tick) {
        metrics_.md_ticks_total->Increment();
        if (trading_paused_ || trading_halted_) return;

        curr_tick_ts_ns_ = tick.timestampNs();

        if (paper_gw_) {
            paper_gw_->feed_trade(tick.instrumentId(),
                                  tick.price(),
                                  tick.qty(),
                                  tick.timestampNs());
        }

        strategy_->on_trade(tick);
        const uint64_t t3 = bpt::common::util::WallClock::now_ns();
        if (t3 > curr_tick_ts_ns_) {
            const uint64_t delta = t3 - curr_tick_ts_ns_;
            tick_lat_hist_.record(delta);
            metrics_.tick_to_strategy_ns_hist->Observe(static_cast<double>(delta));
        }
    };

    md_client_->on_order_book = [this](const bpt::messages::MdOrderBook& book) {
        if (!trading_paused_ && !trading_halted_) {
            curr_tick_ts_ns_ = book.timestampNs();
            strategy_->on_order_book(book);
            const uint64_t t3 = bpt::common::util::WallClock::now_ns();
            if (t3 > curr_tick_ts_ns_) {
                const uint64_t delta = t3 - curr_tick_ts_ns_;
                tick_lat_hist_.record(delta);
                metrics_.tick_to_strategy_ns_hist->Observe(static_cast<double>(delta));
            }
        }
    };
}

void StrategyApp::wire_vol_callbacks() {
    if (!vol_client_) return;

    vol_client_->on_vol_surface = [this](bpt::messages::VolSurface& surface) {
        bpt::common::log::info("VolSurface received: exchange={} underlying={}",
                       ExchangeId::c_str(surface.exchangeId()),
                       surface.getUnderlyingAsString());
        strategy_->on_vol_surface(surface);
    };

    vol_client_->on_ready = [this](uint8_t exchanges_loaded, uint16_t underlying_count, uint32_t point_count) {
        bpt::common::log::info("PricerReady: exchanges=0x{:02x} underlyings={} points={}",
                       exchanges_loaded, underlying_count, point_count);
        pricer_ready_ = true;
    };
}

void StrategyApp::wire_order_callbacks() {
    if (order_mgr_) {
        order_mgr_->on_order_placed = [this](uint64_t /*order_id*/) {
            // curr_tick_ts_ns_ is WallClock-sourced (stamped by bpt-md-gateway and
            // re-read via tick.timestampNs()), so this side must match.
            const uint64_t now = bpt::common::util::WallClock::now_ns();
            if (now <= curr_tick_ts_ns_) return;
            const uint64_t delta = now - curr_tick_ts_ns_;
            order_lat_hist_.record(delta);
            metrics_.tick_to_order_ns_hist->Observe(static_cast<double>(delta));
        };
    }

    if (!order_gw_) return;

    order_gw_->on_exec_report = [this](const bpt::messages::ExecutionReport& rpt) {
        const auto status = rpt.status();
        const double price_d = static_cast<double>(rpt.price()) / 1e8;
        if (status == ExecStatus::ACKED) {
            bpt::common::log::debug("ExecReport order_id={} ACKED price={:.2f}",
                            rpt.orderId(), price_d);
        } else if (status == ExecStatus::REJECTED) {
            bpt::common::log::info("ExecReport order_id={} REJECTED reason={} price={:.2f}",
                           rpt.orderId(), RejectReason::c_str(rpt.rejectReason()), price_d);
        } else {
            bpt::common::log::info("ExecReport order_id={} status={} filled_qty={:.6f} price={:.2f}",
                           rpt.orderId(), ExecStatus::c_str(status),
                           static_cast<double>(rpt.filledQty()) / 1e8, price_d);
        }
        metrics_.exec_reports_total->Increment();
        strategy_->on_exec_report(rpt);
    };

    order_gw_->on_heartbeat = [this](const bpt::messages::OrderGatewayHeartbeat&) {
        last_gw_hb_recv_ns_ = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    };

    order_gw_->on_account_snapshot = [this](bpt::messages::AccountSnapshot& snap) {
        const auto exchange_id = snap.exchangeId();
        bpt::common::log::info(
            "AccountSnapshot received: exchange={} balance={:.2f} positions={}",
            ExchangeId::c_str(exchange_id),
            static_cast<double>(snap.availableBalanceE8()) / 1e8,
            snap.positions().count());

        startup_gate_->on_account_snapshot(exchange_id);
        strategy_->on_account_snapshot(snap);
    };
}

void StrategyApp::run() {
    bpt::common::log::info("Polling... waiting for RefDataReady before subscribing (Ctrl+C to exit)");

    while (bpt::common::signal::is_running()) {
        int frags = refdata_->poll();
        if (md_client_)
            frags += md_client_->poll();
        if (vol_client_)
            frags += vol_client_->poll();
        if (order_gw_)
            frags += order_gw_->poll();
        if (tyr_sub_) {
            frags += tyr_sub_->poll(
                [this](const aeron::concurrent::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       const aeron::Header&) {
                    if (static_cast<std::size_t>(length) != sizeof(bpt::analytics::messaging::ToxicityUpdate))
                        return;
                    bpt::analytics::messaging::ToxicityUpdate update;
                    std::memcpy(&update, buffer.buffer() + offset, sizeof(update));
                    strategy_->on_toxicity_update(update);
                },
                4);
        }

        // Poll dashboard control channel (halt/resume from bridge)
        if (dashboard_ctrl_sub_) {
            frags += dashboard_ctrl_sub_->poll(
                [this](aeron::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       aeron::Header& /*hdr*/) {
                    if (length < 1) return;
                    const uint8_t cmd = *reinterpret_cast<const uint8_t*>(buffer.buffer() + offset);
                    if (cmd == 0x00 && !trading_halted_) {
                        trading_halted_ = true;
                        bpt::common::log::warn("TRADING HALTED via dashboard kill-switch");
                    } else if (cmd == 0x01 && trading_halted_) {
                        trading_halted_ = false;
                        bpt::common::log::info("Trading RESUMED via dashboard");
                    }
                },
                1);
        }

        startup_gate_->tick();

        // Once the strategy has its snapshot and started MD subscriptions, switch to
        // tick-gating mode if running a backtest.
        if (cfg_.backtest_mode && startup_gate_->is_active()) {
            run_backtest_loop();
            break;
        }

        if (startup_gate_->is_active()) {
            check_service_liveness();
            report_latency_stats();

            if (portfolio_snap_pub_ && portfolio_snap_pub_->is_active()) {
                const auto now_ns = bpt::common::util::TscClock::now_epoch_ns();
                portfolio_snap_pub_->publish_if_due(strategy_->get_portfolio_state(), now_ns);

                // Publish strategy state at ~2 Hz (every 500ms).
                static uint64_t last_state_ns = 0;
                if (now_ns - last_state_ns > 500'000'000ULL) {
                    last_state_ns = now_ns;
                    auto state_json = strategy_->get_strategy_state_json();
                    if (!state_json.empty()) {
                        aeron::AtomicBuffer buf(reinterpret_cast<uint8_t*>(state_json.data()),
                                                static_cast<aeron::util::index_t>(state_json.size()));
                        // Reuse the same publication as portfolio snapshots —
                        // the bridge relays all JSON on stream 9004 to WS clients.
                        portfolio_snap_pub_->offer_raw(buf, static_cast<int32_t>(state_json.size()));
                    }
                }
            }
        }

        if (frags == 0)
            __builtin_ia32_pause();
    }

}

void StrategyApp::stop() {
    // Called by bpt::app::run() after run() exits on signal. Graceful
    // shutdown does two things in order: (1) cancel resting orders and
    // flatten inventory while the dashboard can still observe, (2) drain
    // the Prometheus exposer. Both are symmetric with startup side-effects.
    shutdown_flatten();
    metrics_.shutdown();
}

void StrategyApp::shutdown_flatten() {
    if (!strategy_) {
        return;
    }
    bpt::common::log::warn("Shutdown flatten starting — cancelling resting orders and closing open positions");

    // Pre-drain: process any exec reports already queued on the gateway
    // BEFORE we ask the strategy to flatten. If a fill happened in the
    // last few ms of the main loop, it's still in flight and the
    // strategy's internal net_qty is stale — it would see net_qty=0 and
    // not fire an unwind, leaving a real exchange-side position open.
    // A short pre-drain lets PositionTracker catch up to reality. Only
    // poll the order gateway — fresh MD ticks would re-invoke strategy
    // handlers and could fire new entries.
    if (order_gw_) {
        const uint64_t pre_drain_start = bpt::common::util::TscClock::now_epoch_ns();
        constexpr uint64_t kPreDrainBudgetNs = 1'000'000'000ULL;
        while (bpt::common::util::TscClock::now_epoch_ns() - pre_drain_start < kPreDrainBudgetNs) {
            const int frags = order_gw_->poll();
            if (frags == 0)
                __builtin_ia32_pause();
        }
    }

    try {
        strategy_->on_shutdown_flatten();
    } catch (const std::exception& e) {
        bpt::common::log::error("on_shutdown_flatten threw: {}", e.what());
    }

    // Post-drain: loop until the strategy says all its
    // cancels/unwinds have reached a terminal status, or until the
    // drain budget expires. Previous implementation sleep-spun the
    // full 5s regardless of whether orders had already ack'd — fine
    // in the happy path, wasteful; and worse, if the exchange was
    // slow (network hiccup, adapter reconnect mid-flatten) the budget
    // could expire with orders still live. We now log clearly when
    // the budget expires so ops know to look.
    const uint64_t drain_start_ns = bpt::common::util::TscClock::now_epoch_ns();
    constexpr uint64_t kDrainBudgetNs = 5ULL * 1'000'000'000ULL;
    bool drained_cleanly = true;
    if (order_gw_) {
        while (true) {
            const uint64_t elapsed = bpt::common::util::TscClock::now_epoch_ns() - drain_start_ns;
            if (elapsed >= kDrainBudgetNs) {
                if (strategy_->has_pending_flatten()) {
                    bpt::common::log::error(
                        "shutdown drain budget ({} ms) expired with pending "
                        "orders still in flight — process exiting with live exchange-side "
                        "state. Investigate via order-gateway logs and exchange console.",
                        kDrainBudgetNs / 1'000'000ULL);
                    drained_cleanly = false;
                }
                break;
            }
            if (!strategy_->has_pending_flatten()) {
                bpt::common::log::info("shutdown drain completed cleanly in {} ms",
                               elapsed / 1'000'000ULL);
                break;
            }
            const int frags = order_gw_->poll();
            if (frags == 0)
                __builtin_ia32_pause();
        }
    }
    (void)drained_cleanly;  // reserved: may gate exit code in the future

    // Request a fresh AccountSnapshot from every configured exchange so the
    // dashboard HoldingsPanel reflects the post-flatten state (instead of
    // waiting up to the periodic republish interval in order-gateway).
    if (startup_gate_) {
        try {
            startup_gate_->refresh_account_snapshots();
        } catch (const std::exception& e) {
            bpt::common::log::error("refresh_account_snapshots threw: {}", e.what());
        }
    }

    // Brief secondary drain so the refresh snapshot propagates through the
    // bus before we exit.
    if (order_gw_) {
        const uint64_t t1 = bpt::common::util::TscClock::now_epoch_ns();
        constexpr uint64_t kSnapDrainBudgetNs = 1'000'000'000ULL;
        while (bpt::common::util::TscClock::now_epoch_ns() - t1 < kSnapDrainBudgetNs) {
            const int frags = order_gw_->poll();
            if (frags == 0)
                __builtin_ia32_pause();
        }
    }

    // Warm-start save: EWMA / regime state depends only on market data
    // (not our positions), so saving is safe even when the drain didn't
    // complete cleanly. The TTL on load is the real safety net against
    // a stale restart.
    const auto& ws = cfg_.strat.strategy.warm_start;
    if (!ws.state_dir.empty()) {
        const auto path = std::filesystem::path(ws.state_dir) /
                          (std::to_string(cfg_.strat.correlation_id) + ".json");
        try {
            strategy_->save_state(path.string());
        } catch (const std::exception& e) {
            bpt::common::log::error("save_state threw: {}", e.what());
        }
    }

    bpt::common::log::warn("Shutdown flatten complete");
}

void StrategyApp::check_service_liveness() {
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());

    // Throttle to once per second — no need to check every poll iteration.
    if (now_ns - last_liveness_check_ns_ < 1'000'000'000ULL)
        return;
    last_liveness_check_ns_ = now_ns;

    const uint64_t threshold_ns = cfg_.strat.strategy.schedule.md_staleness_threshold_ms * 1'000'000ULL;
    bool stale = false;

    if (md_client_ && last_md_hb_recv_ns_ != 0) {
        const uint64_t age_ns = now_ns - last_md_hb_recv_ns_;
        if (age_ns > threshold_ns) {
            bpt::common::log::warn("MD Gateway heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing trading",
                           age_ns / 1e9,
                           threshold_ns / 1e9);
            stale = true;
        }
    }

    if (order_gw_ && last_gw_hb_recv_ns_ != 0) {
        const uint64_t age_ns = now_ns - last_gw_hb_recv_ns_;
        if (age_ns > threshold_ns) {
            bpt::common::log::warn("OrderGateway heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing trading",
                           age_ns / 1e9,
                           threshold_ns / 1e9);
            stale = true;
        }
    }

    if (stale && !trading_paused_) {
        trading_paused_ = true;
        metrics_.trading_paused->Set(1.0);
        bpt::common::log::warn("Trading PAUSED — service heartbeat(s) stale");
    } else if (!stale && trading_paused_) {
        trading_paused_ = false;
        metrics_.trading_paused->Set(0.0);
        bpt::common::log::info("Trading RESUMED — all service heartbeats healthy");
    }
}

void StrategyApp::report_latency_stats() {
    constexpr uint64_t kReportIntervalNs = 30'000'000'000ULL;  // 30 s

    const uint64_t now = bpt::common::util::TscClock::now_epoch_ns();
    if (now - last_lat_report_ns_ < kReportIntervalNs)
        return;
    last_lat_report_ns_ = now;

    auto tick = tick_lat_hist_.snapshot_and_reset();
    auto ord = order_lat_hist_.snapshot_and_reset();

    if (tick.total == 0) {
        bpt::common::log::info("[Latency] No MD ticks processed in last 30s");
        return;
    }

    // T0 = bpt-md-gateway TSC at wire receipt; T3 = strategy TSC after strategy returns.
    // Both services calibrate TscClock independently — cross-process skew is
    // typically <1µs on a single host with invariant TSC, so delta is valid.
    bpt::common::log::info(
        "[Latency] MD tick→strategy (T0→T3): n={} "
        "p50={:.1f}µs p90={:.1f}µs p99={:.1f}µs p99.9={:.1f}µs max={:.1f}µs mean={:.1f}µs",
        tick.total,
        tick.percentile_ns(0.50) / 1e3,
        tick.percentile_ns(0.90) / 1e3,
        tick.percentile_ns(0.99) / 1e3,
        tick.percentile_ns(0.999) / 1e3,
        tick.max_ns() / 1e3,
        tick.mean_ns() / 1e3);

    if (ord.total > 0) {
        bpt::common::log::info(
            "[Latency] MD tick→order placed (T0→T3 w/order): n={} "
            "p50={:.1f}µs p90={:.1f}µs p99={:.1f}µs max={:.1f}µs mean={:.1f}µs",
            ord.total,
            ord.percentile_ns(0.50) / 1e3,
            ord.percentile_ns(0.90) / 1e3,
            ord.percentile_ns(0.99) / 1e3,
            ord.max_ns() / 1e3,
            ord.mean_ns() / 1e3);
    } else {
        bpt::common::log::info("[Latency] No orders placed in last 30s");
    }
}

void StrategyApp::run_backtest_loop() {
    bpt::common::log::info("Backtest: strategy ready — entering tick-gating loop");

    bool stop_received = false;

    backtest_client_->on_control =
        [this, &stop_received](bpt::messages::BacktestCommand::Value cmd, uint64_t seq, uint64_t sim_ts) {
            using bpt::messages::BacktestCommand;

            if (cmd == BacktestCommand::START) {
                if (seq == 0) {
                    // Initial handshake — Backtester is ready to start ticking.
                    bpt::common::log::info("Backtest handshake received, acking");
                    backtest_client_->send_ack(0, 0);
                } else {
                    // Normal tick — drain MD and execution reports for up to 10 ms,
                    // then signal Backtester to advance to the next tick.
                    auto deadline = std::chrono::steady_clock::now() + 10ms;
                    int drained = 0;
                    while (std::chrono::steady_clock::now() < deadline) {
                        int f = 0;
                        if (md_client_)
                            f += md_client_->poll();
                        if (order_gw_)
                            f += order_gw_->poll();
                        drained += f;
                        if (f == 0 && drained > 0)
                            break;  // drained everything, stop early
                        if (f == 0)
                            std::this_thread::sleep_for(100us);
                    }
                    backtest_client_->send_ack(seq, sim_ts);
                }
            } else if (cmd == BacktestCommand::STOP) {
                bpt::common::log::info("Backtest STOP received — halting");
                stop_received = true;
            }
        };

    while (!stop_received && bpt::common::signal::is_running()) {
        int frags = backtest_client_->poll();
        if (frags == 0)
            std::this_thread::sleep_for(10us);
    }
}

}  // namespace bpt::strategy

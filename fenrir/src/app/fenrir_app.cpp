#include "fenrir/app/fenrir_app.h"

#include "fenrir/strategy/strategy_factory.h"

#include <bifrost_protocol/BacktestCommand.h>
#include <bifrost_protocol/ExchangeId.h>
#include <bifrost_protocol/ExecStatus.h>
#include <bifrost_protocol/RejectReason.h>

#include <chrono>
#include <thread>
#include <yggdrasil/signal.h>

using namespace std::chrono_literals;
using bifrost::protocol::ExchangeId;
using bifrost::protocol::ExecStatus;
using bifrost::protocol::RefDataErrorType;
using bifrost::protocol::RejectReason;

namespace fenrir {

FenrirApp::FenrirApp(config::AppConfig cfg, std::shared_ptr<aeron::Aeron> aeron)
    : cfg_(std::move(cfg)),
      aeron_(aeron),
      metrics_(cfg_.metrics_port),
      fee_cache_(cfg_.fenrir.strategy.schedule.max_refdata_staleness_ns),
      funding_rate_cache_(cfg_.fenrir.strategy.schedule.max_refdata_staleness_ns) {
    const auto& ac = cfg_.aeron;
    const auto& fc = cfg_.fenrir;

    refdata_ = std::make_unique<refdata::RefdataClient>(aeron,
                                                        ac.refdata_control.channel,
                                                        ac.refdata_control.stream_id,
                                                        ac.refdata_snapshot.stream_id,
                                                        ac.refdata_delta.stream_id,
                                                        ac.fee_schedule.stream_id,
                                                        ac.funding_rate.stream_id,
                                                        ac.refdata_status.stream_id,
                                                        fee_cache_,
                                                        funding_rate_cache_,
                                                        ac.pub_timeout_ms,
                                                        ac.pub_poll_interval_ms);

    if (ac.md_control.stream_id != 0) {
        md_client_ = std::make_unique<md::MdClient>(aeron,
                                                    ac.md_control.channel,
                                                    ac.md_control.stream_id,
                                                    ac.md_data.stream_id,
                                                    ac.md_ack_hb.stream_id,
                                                    ac.pub_timeout_ms,
                                                    ac.pub_poll_interval_ms);
    }

    if (ac.order.stream_id != 0) {
        order_gw_ = std::make_unique<order::OrderGatewayClient>(aeron,
                                                                ac.order.channel,
                                                                ac.order.stream_id,
                                                                ac.exec_report.stream_id,
                                                                ac.heartbeat.stream_id,
                                                                ac.account_snapshot.stream_id,
                                                                ac.pub_timeout_ms,
                                                                ac.pub_poll_interval_ms);
    }

    if (ac.vol_surface.stream_id != 0) {
        vol_client_ = std::make_unique<vol::VolSurfaceClient>(aeron,
                                                              ac.vol_surface.channel,
                                                              ac.vol_surface.stream_id,
                                                              ac.surtr_status.stream_id,
                                                              ac.pub_timeout_ms,
                                                              ac.pub_poll_interval_ms);
        ygg::log::info("[Fenrir] VolSurfaceClient ready: surface={} status={}",
                       ac.vol_surface.stream_id,
                       ac.surtr_status.stream_id);
    }

    if (order_gw_) {
        order_mgr_ = std::make_unique<order::OrderManager>(*order_gw_, refdata_->cache());
    }

    strategy_ = strategy::StrategyFactory::create(fc, *refdata_, md_client_.get(), order_mgr_.get(), vol_client_.get());
    wire_callbacks();

    if (cfg_.backtest_mode) {
        backtest_client_ =
            std::make_unique<backtest::BacktestClient>(aeron,
                                                       ac.backtest_control.channel,
                                                       ac.backtest_control.stream_id,  // sub: Jormungandr → Fenrir
                                                       ac.backtest_ack.stream_id,      // pub: Fenrir → Jormungandr
                                                       ac.pub_timeout_ms,
                                                       ac.pub_poll_interval_ms);
        ygg::log::info("[Fenrir] Backtest mode enabled: ctrl_sub={} ack_pub={}",
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
            ygg::log::info("[Fenrir] Dashboard control subscription ready on stream {}",
                           ac.dashboard_control.stream_id);
        } else {
            ygg::log::warn("[Fenrir] Dashboard control subscription unavailable");
        }
    }
}

void FenrirApp::wire_callbacks() {
    refdata_->on_ready = [this](uint8_t exchanges_loaded,
                                uint16_t instrument_count,
                                bool fee_schedules_loaded,
                                bool funding_rates_loaded) {
        const uint8_t configured_mask = cfg_.fenrir.strategy.schedule.configured_exchanges_mask;
        if (!refdata_ready_) {
            ygg::log::info(
                "[Fenrir] RefDataReady: exchanges=0x{:02x} (configured=0x{:02x}) "
                "instruments={} fee_schedules={} funding_rates={}",
                exchanges_loaded,
                configured_mask,
                instrument_count,
                fee_schedules_loaded,
                funding_rates_loaded);
        } else {
            ygg::log::debug("[Fenrir] RefDataReady (periodic): exchanges=0x{:02x} instruments={}",
                            exchanges_loaded,
                            instrument_count);
        }

        if (configured_mask != 0 && (exchanges_loaded & configured_mask) != configured_mask) {
            const uint8_t missing = configured_mask & ~exchanges_loaded;
            ygg::log::critical(
                "[Fenrir] HALT — refdata service is missing required exchanges (mask=0x{:02x}). "
                "Cannot trade safely. Shutting down.",
                missing);
            ygg::signal::stop();
        } else {
            refdata_ready_ = true;
            if (exchanges_loaded & 0x01)
                metrics_.refdata_ready("BINANCE").Set(1.0);
            if (exchanges_loaded & 0x02)
                metrics_.refdata_ready("OKX").Set(1.0);
            if (exchanges_loaded & 0x04)
                metrics_.refdata_ready("HYPERLIQUID").Set(1.0);
            if (exchanges_loaded & 0x08)
                metrics_.refdata_ready("DERIBIT").Set(1.0);
        }
    };

    refdata_->on_error = [](RefDataErrorType::Value error_type, ExchangeId::Value exchange_id, uint64_t instrument_id) {
        ygg::log::error("[Fenrir] RefDataError: type={} exchange={} instrument={}",
                        RefDataErrorType::c_str(error_type),
                        ExchangeId::c_str(exchange_id),
                        instrument_id);
    };

    refdata_->on_snapshot_complete = [this](const refdata::InstrumentCache& cache) {
        strategy_->on_snapshot(cache);
        if (strategy_started_ && !strategy_md_started_) {
            strategy_md_started_ = true;
            ygg::log::info("[Fenrir] Snapshot received — starting strategy MD subscriptions");
            strategy_->start();
        }
    };

    refdata_->on_delta = [this](const refdata::Instrument& inst,
                                bifrost::protocol::DeltaUpdateType::Value update_type) {
        strategy_->on_delta(inst, update_type);
    };

    if (md_client_) {
        md_client_->on_service_heartbeat = [this]() {
            last_md_hb_recv_ns_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                            std::chrono::steady_clock::now().time_since_epoch())
                                                            .count());
        };
        md_client_->on_bbo = [this](const bifrost::protocol::MdMarketData& tick) {
            static uint64_t bbo_count = 0;
            if (++bbo_count <= 10 || bbo_count % 1000 == 0) {
                ygg::log::info("[Fenrir] BBO tick #{}: id={} bid={:.4f} ask={:.4f}",
                               bbo_count,
                               tick.instrumentId(),
                               tick.bidPrice(),
                               tick.askPrice());
            }
            metrics_.md_ticks_total->Increment();
            if (!trading_paused_ && !trading_halted_) {
                curr_tick_ts_ns_ = tick.timestampNs();
                strategy_->on_bbo(tick);
                const uint64_t t3 = ygg::util::TscClock::now_epoch_ns();
                if (t3 > curr_tick_ts_ns_) {
                    const uint64_t delta = t3 - curr_tick_ts_ns_;
                    if (bbo_count <= 3)
                        ygg::log::debug("[Latency] bbo raw delta={}ns", delta);
                    tick_lat_hist_.record(delta);
                }
            }
        };
        md_client_->on_trade = [this](const bifrost::protocol::MdTrade& tick) {
            metrics_.md_ticks_total->Increment();
            if (!trading_paused_ && !trading_halted_) {
                curr_tick_ts_ns_ = tick.timestampNs();
                strategy_->on_trade(tick);
                const uint64_t t3 = ygg::util::TscClock::now_epoch_ns();
                if (t3 > curr_tick_ts_ns_)
                    tick_lat_hist_.record(t3 - curr_tick_ts_ns_);
            }
        };
        md_client_->on_order_book = [this](const bifrost::protocol::MdOrderBook& book) {
            if (!trading_paused_ && !trading_halted_) {
                curr_tick_ts_ns_ = book.timestampNs();
                strategy_->on_order_book(book);
                const uint64_t t3 = ygg::util::TscClock::now_epoch_ns();
                if (t3 > curr_tick_ts_ns_)
                    tick_lat_hist_.record(t3 - curr_tick_ts_ns_);
            }
        };
    }

    if (vol_client_) {
        vol_client_->on_vol_surface = [this](bifrost::protocol::VolSurface& surface) {
            ygg::log::info("[Fenrir] VolSurface received: exchange={} underlying={}",
                           ExchangeId::c_str(surface.exchangeId()),
                           surface.getUnderlyingAsString());
            strategy_->on_vol_surface(surface);
        };
        vol_client_->on_ready = [this](uint8_t exchanges_loaded, uint16_t underlying_count, uint32_t point_count) {
            ygg::log::info("[Fenrir] SurtrReady: exchanges=0x{:02x} underlyings={} points={}",
                           exchanges_loaded,
                           underlying_count,
                           point_count);
            surtr_ready_ = true;
        };
    }

    if (order_mgr_) {
        order_mgr_->on_order_placed = [this](uint64_t /*order_id*/) {
            order_lat_hist_.record(ygg::util::TscClock::now_epoch_ns() - curr_tick_ts_ns_);
        };
    }

    if (order_gw_) {
        order_gw_->on_exec_report = [this](const bifrost::protocol::ExecutionReport& rpt) {
            const auto status = rpt.status();
            if (status == ExecStatus::ACKED) {
                ygg::log::debug("[Fenrir] ExecReport order_id={} ACKED price={:.2f}",
                                rpt.orderId(),
                                static_cast<double>(rpt.price()) / 1e8);
            } else if (status == ExecStatus::REJECTED) {
                ygg::log::info("[Fenrir] ExecReport order_id={} REJECTED reason={} price={:.2f}",
                               rpt.orderId(),
                               RejectReason::c_str(rpt.rejectReason()),
                               static_cast<double>(rpt.price()) / 1e8);
            } else {
                ygg::log::info("[Fenrir] ExecReport order_id={} status={} filled_qty={:.6f} price={:.2f}",
                               rpt.orderId(),
                               ExecStatus::c_str(status),
                               static_cast<double>(rpt.filledQty()) / 1e8,
                               static_cast<double>(rpt.price()) / 1e8);
            }
            metrics_.exec_reports_total->Increment();
            strategy_->on_exec_report(rpt);
        };

        order_gw_->on_heartbeat = [this](const bifrost::protocol::OrderGatewayHeartbeat&) {
            last_gw_hb_recv_ns_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                            std::chrono::steady_clock::now().time_since_epoch())
                                                            .count());
        };

        order_gw_->on_account_snapshot = [this](bifrost::protocol::AccountSnapshot& snap) {
            const auto exchange_id = snap.exchangeId();
            uint8_t bit = 0;
            if (exchange_id == ExchangeId::BINANCE)
                bit = 0x01;
            else if (exchange_id == ExchangeId::OKX)
                bit = 0x02;
            else if (exchange_id == ExchangeId::HYPERLIQUID)
                bit = 0x04;

            account_snapshot_ready_ |= bit;

            ygg::log::info(
                "[Fenrir] AccountSnapshot received: exchange={} balance={:.2f} positions={} "
                "ready_mask=0x{:02x}",
                ExchangeId::c_str(exchange_id),
                static_cast<double>(snap.availableBalanceE8()) / 1e8,
                snap.positions().count(),
                account_snapshot_ready_);

            strategy_->on_account_snapshot(snap);
        };
    }
}

void FenrirApp::run() {
    ygg::log::info("Polling... waiting for RefDataReady before subscribing (Ctrl+C to exit)");

    while (ygg::signal::is_running()) {
        int frags = refdata_->poll();
        if (md_client_)
            frags += md_client_->poll();
        if (vol_client_)
            frags += vol_client_->poll();
        if (order_gw_)
            frags += order_gw_->poll();

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
                        ygg::log::warn("[Fenrir] TRADING HALTED via dashboard kill-switch");
                    } else if (cmd == 0x01 && trading_halted_) {
                        trading_halted_ = false;
                        ygg::log::info("[Fenrir] Trading RESUMED via dashboard");
                    }
                },
                1);
        }

        // Step 1: once refdata is ready, send AccountSnapshotRequests to Heimdall
        // (one per configured exchange). This runs before the refdata subscription
        // so position state is seeded before the strategy sees any ticks.
        if (refdata_ready_ && order_gw_ && !account_snapshot_requests_sent_) {
            account_snapshot_requests_sent_ = true;
            const uint8_t mask = cfg_.fenrir.strategy.schedule.configured_exchanges_mask;
            uint64_t corr = cfg_.fenrir.correlation_id;
            if (mask & 0x01) {
                ygg::log::info("[Fenrir] Requesting AccountSnapshot for BINANCE");
                order_gw_->send_account_snapshot_request(ExchangeId::BINANCE, corr++);
            }
            if (mask & 0x02) {
                ygg::log::info("[Fenrir] Requesting AccountSnapshot for OKX");
                order_gw_->send_account_snapshot_request(ExchangeId::OKX, corr++);
            }
            if (mask & 0x04) {
                ygg::log::info("[Fenrir] Requesting AccountSnapshot for HYPERLIQUID");
                order_gw_->send_account_snapshot_request(ExchangeId::HYPERLIQUID, corr++);
            }
        }

        // Step 2: wait for AccountSnapshot for all configured exchanges before subscribing.
        // If order_gw_ is not configured, skip the gate (refdata-only mode).
        const uint8_t configured_mask = cfg_.fenrir.strategy.schedule.configured_exchanges_mask;
        const bool account_ready = !order_gw_ || (account_snapshot_ready_ & configured_mask) == configured_mask;

        if (!strategy_started_ && refdata_ready_ && account_ready) {
            strategy_started_ = true;
            metrics_.strategy_active->Set(1.0);
            ygg::log::info("[Fenrir] RefDataReady + AccountSnapshot received — sending RefDataSubscriptionRequest");
            refdata_->subscribe(cfg_.fenrir.correlation_id);
        }

        // Once the strategy has its snapshot and started MD subscriptions, switch to
        // tick-gating mode if running a backtest.
        if (cfg_.backtest_mode && strategy_md_started_) {
            run_backtest_loop();
            break;
        }

        if (strategy_started_) {
            check_service_liveness();
            report_latency_stats();
        }

        if (frags == 0)
            __builtin_ia32_pause();
    }

    metrics_.shutdown();
    ygg::log::info("Shutting down");
}

void FenrirApp::check_service_liveness() {
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());

    // Throttle to once per second — no need to check every poll iteration.
    if (now_ns - last_liveness_check_ns_ < 1'000'000'000ULL)
        return;
    last_liveness_check_ns_ = now_ns;

    const uint64_t threshold_ns = cfg_.fenrir.strategy.schedule.md_staleness_threshold_ms * 1'000'000ULL;
    bool stale = false;

    if (md_client_ && last_md_hb_recv_ns_ != 0) {
        const uint64_t age_ns = now_ns - last_md_hb_recv_ns_;
        if (age_ns > threshold_ns) {
            ygg::log::warn("[Fenrir] Huginn heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing trading",
                           age_ns / 1e9,
                           threshold_ns / 1e9);
            stale = true;
        }
    }

    if (order_gw_ && last_gw_hb_recv_ns_ != 0) {
        const uint64_t age_ns = now_ns - last_gw_hb_recv_ns_;
        if (age_ns > threshold_ns) {
            ygg::log::warn("[Fenrir] Heimdall heartbeat stale ({:.1f}s, threshold={:.1f}s) — pausing trading",
                           age_ns / 1e9,
                           threshold_ns / 1e9);
            stale = true;
        }
    }

    if (stale && !trading_paused_) {
        trading_paused_ = true;
        metrics_.trading_paused->Set(1.0);
        ygg::log::warn("[Fenrir] Trading PAUSED — service heartbeat(s) stale");
    } else if (!stale && trading_paused_) {
        trading_paused_ = false;
        metrics_.trading_paused->Set(0.0);
        ygg::log::info("[Fenrir] Trading RESUMED — all service heartbeats healthy");
    }
}

void FenrirApp::report_latency_stats() {
    constexpr uint64_t kReportIntervalNs = 30'000'000'000ULL;  // 30 s

    const uint64_t now = ygg::util::TscClock::now_epoch_ns();
    if (now - last_lat_report_ns_ < kReportIntervalNs)
        return;
    last_lat_report_ns_ = now;

    auto tick = tick_lat_hist_.snapshot_and_reset();
    auto ord = order_lat_hist_.snapshot_and_reset();

    if (tick.total == 0) {
        ygg::log::info("[Latency] No MD ticks processed in last 30s");
        return;
    }

    // T0 = huginn TSC at wire receipt; T3 = fenrir TSC after strategy returns.
    // Both services calibrate TscClock independently — cross-process skew is
    // typically <1µs on a single host with invariant TSC, so delta is valid.
    ygg::log::info(
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
        ygg::log::info(
            "[Latency] MD tick→order placed (T0→T3 w/order): n={} "
            "p50={:.1f}µs p90={:.1f}µs p99={:.1f}µs max={:.1f}µs mean={:.1f}µs",
            ord.total,
            ord.percentile_ns(0.50) / 1e3,
            ord.percentile_ns(0.90) / 1e3,
            ord.percentile_ns(0.99) / 1e3,
            ord.max_ns() / 1e3,
            ord.mean_ns() / 1e3);
    } else {
        ygg::log::info("[Latency] No orders placed in last 30s");
    }
}

void FenrirApp::run_backtest_loop() {
    ygg::log::info("[Fenrir] Backtest: strategy ready — entering tick-gating loop");

    bool stop_received = false;

    backtest_client_->on_control =
        [this, &stop_received](bifrost::protocol::BacktestCommand::Value cmd, uint64_t seq, uint64_t sim_ts) {
            using bifrost::protocol::BacktestCommand;

            if (cmd == BacktestCommand::START) {
                if (seq == 0) {
                    // Initial handshake — Jormungandr is ready to start ticking.
                    ygg::log::info("[Fenrir] Backtest handshake received, acking");
                    backtest_client_->send_ack(0, 0);
                } else {
                    // Normal tick — drain MD and execution reports for up to 10 ms,
                    // then signal Jormungandr to advance to the next tick.
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
                ygg::log::info("[Fenrir] Backtest STOP received — halting");
                stop_received = true;
            }
        };

    while (!stop_received && ygg::signal::is_running()) {
        int frags = backtest_client_->poll();
        if (frags == 0)
            std::this_thread::sleep_for(10us);
    }
}

}  // namespace fenrir

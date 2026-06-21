#include "md_gateway/app/md_gateway_service.h"

#include "md_gateway/adapter/common/md_adapter_factory.h"
#include "md_gateway/md/validation_drop_breaker.h"
#include "md_gateway/messaging/publishers/aeron/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/aeron/instrument_stats_publisher.h"
#include "md_gateway/messaging/publishers/md_publisher.h"

#include <messages/ExchangeRegistry.h>
#include <messages/MdSubscribeBatch.h>

#include <bpt_common/signal.h>
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

namespace bpt::md_gateway {

namespace {

md::ValidationDropBreaker::Config breaker_cfg_from(const config::AdapterConfig& a) {
    md::ValidationDropBreaker::Config db;
    db.enabled = a.validation_drop_breaker_enabled;
    db.threshold_pct = a.validation_drop_threshold_pct;
    db.window_ns = static_cast<uint64_t>(a.validation_drop_window_sec) * 1'000'000'000ULL;
    db.min_events = a.validation_drop_min_events;
    return db;
}

}  // namespace

MdGatewayService::MdGatewayService(config::Settings cfg,
                                   std::shared_ptr<::aeron::Aeron> aeron,
                                   std::unique_ptr<messaging::api::MdControlSubscriber> control_sub,
                                   std::unique_ptr<messaging::api::AckPublisher> ack_pub,
                                   const bpt::common::util::Topology& topology)
    : cfg_(std::move(cfg)),
      metrics_(cfg_.metrics_host, cfg_.base.metrics_port),
      ack_pub_(std::move(ack_pub)),
      ctrl_sub_(std::move(control_sub)) {
    for (const auto& a_cfg : cfg_.adapters) {
        // Validate against ExchangeRegistry at the boundary so a typo in
        // the TOML fails immediately rather than skipping silently with a
        // warning. The registry is YAML-driven; new venues land here when
        // messages/exchanges.yaml is edited and codegen rerun.
        const auto exch_id = bpt::messages::ExchangeRegistry::from_name(a_cfg.exchange);
        if (!exch_id) {
            throw std::runtime_error(
                fmt::format("Unknown exchange '{}' in mdgw config — not in messages/exchanges.yaml", a_cfg.exchange));
        }
        // Per-adapter publishers. Each venue owns its own Aeron publications
        // on md_data, funding_rate, and instrument_stats. Aeron supports
        // N publications on the same (channel, stream_id) natively — the
        // subscriber sees N session-ids interleaved. MdPublisher's validator
        // state is publisher-thread-confined; funding/stats publishers
        // could in principle be shared but per-adapter ownership matches
        // the "adapter owns what it produces" greenfield principle and
        // gives free per-venue metrics granularity.
        auto md_pub = std::make_shared<messaging::MdPublisher>(aeron,
                                                               cfg_.aeron.md_data,
                                                               a_cfg.max_price_deviation_pct,
                                                               breaker_cfg_from(a_cfg),
                                                               a_cfg.exchange);
        auto funding_pub = std::make_shared<messaging::aeron::FundingRatePublisher>(aeron, cfg_.aeron.funding_rate);
        auto stats_pub =
            std::make_shared<messaging::aeron::InstrumentStatsPublisher>(aeron, cfg_.aeron.instrument_stats);

        auto adapter = adapter::make_md_adapter<messaging::MdPublisher>(*exch_id,
                                                                        a_cfg,
                                                                        std::move(md_pub),
                                                                        std::move(funding_pub),
                                                                        std::move(stats_pub));
        if (!adapter) {
            throw std::runtime_error(
                fmt::format("Exchange '{}' is in the registry but mdgw has no adapter implementation for it",
                            a_cfg.exchange));
        }

        // Use raw pointers into the metrics families — these are stable for the
        // lifetime of MdGatewayService (metrics_ is a member).
        prometheus::Gauge* connected_gauge = &metrics_.exchange_connected(a_cfg.exchange);
        prometheus::Counter* disconnect_ctr = &metrics_.adapter_disconnects(a_cfg.exchange);
        connected_gauge->Set(0.0);  // initialise to 0 before the thread connects

        adapter->on_connect = [connected_gauge]() {
            connected_gauge->Set(1.0);
        };
        adapter->on_disconnect = [connected_gauge, disconnect_ctr]() {
            connected_gauge->Set(0.0);
            disconnect_ctr->Increment();
        };

        lat_reporters_.emplace_back(a_cfg.exchange, &adapter->decode_latency_hist());
        md_stat_reporters_.emplace_back(a_cfg.exchange, adapter.get());
        // Set topology before start() — run() reads it on first line and
        // late-binding a pin after thread launch is racy.
        adapter->set_topology(topology);
        adapter->start();
        sub_mgr_.add_adapter(std::move(adapter));
    }

    bpt::common::log::info("MdGateway ready — polling control stream {}", cfg_.aeron.md_control.stream_id);
}

void MdGatewayService::run() {
    const auto svc_hb_interval = std::chrono::milliseconds(cfg_.service_heartbeat_interval_ms);
    const auto sub_hb_interval = std::chrono::milliseconds(cfg_.subscription_heartbeat_interval_ms);
    const auto lat_report_interval = std::chrono::seconds(5);

    auto last_svc_hb = std::chrono::steady_clock::now();
    auto last_sub_hb = std::chrono::steady_clock::now();
    auto last_lat_report = std::chrono::steady_clock::now();

    while (bpt::common::signal::is_running()) {
        int fragments = ctrl_sub_->poll([this](bpt::messages::MdSubscribeBatch& msg) {
            bpt::common::log::info("Received MdSubscribeBatch correlation_id={}", msg.correlationId());
            sub_mgr_.apply_batch(msg, *ack_pub_);
            metrics_.subscription_batches_total->Increment();
        });

        auto now = std::chrono::steady_clock::now();

        if (now - last_svc_hb >= svc_hb_interval) {
            ack_pub_->publish_service_heartbeat();
            metrics_.service_heartbeats_total->Increment();
            last_svc_hb = now;
        }

        if (now - last_sub_hb >= sub_hb_interval) {
            sub_mgr_.publish_subscription_heartbeats(*ack_pub_);
            last_sub_hb = now;
        }

        if (fragments == 0)
            std::this_thread::sleep_for(10us);

        uint64_t total_backpressure_drops = 0;
        for (auto& [_, a] : md_stat_reporters_)
            total_backpressure_drops += a->md_backpressure_drop_count();
        if (total_backpressure_drops > last_backpressure_drops_) {
            metrics_.md_messages_dropped->Increment(
                static_cast<double>(total_backpressure_drops - last_backpressure_drops_));
            last_backpressure_drops_ = total_backpressure_drops;
        }

        // Snapshot decode latency histograms and push per-exchange gauges to Prometheus.
        if (now - last_lat_report >= lat_report_interval) {
            for (auto& [exchange, hist] : lat_reporters_)
                metrics_.update_decode_latency(exchange, *hist);
            for (auto& [exchange, a] : md_stat_reporters_) {
                metrics_.md_messages_published(exchange).Set(static_cast<double>(a->md_published_count()));
                metrics_.md_validation_drops(exchange).Set(static_cast<double>(a->validation_drop_count()));
                metrics_.validation_drop_breaker_tripped(exchange).Set(a->validation_drop_breaker_tripped() ? 1.0
                                                                                                            : 0.0);
            }
            last_lat_report = now;
        }
    }
}

void MdGatewayService::stop() {
    // Called by bpt::app::run() after our run() loop exits on signal.
    // Adapter WS threads + Prometheus exposer are drained here so the
    // startup-log side-effects remain symmetric with teardown.
    sub_mgr_.stop_all();
    metrics_.shutdown();
}

}  // namespace bpt::md_gateway

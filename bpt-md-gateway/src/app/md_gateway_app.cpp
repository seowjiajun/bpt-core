#include "md_gateway/app/md_gateway_app.h"

#include "md_gateway/adapter/common/md_adapter_factory.h"

#include <messages/ExchangeRegistry.h>
#include <messages/MdSubscribeBatch.h>

#include <chrono>
#include <thread>
#include <bpt_common/signal.h>

using namespace std::chrono_literals;

namespace bpt::md_gateway {

MdGatewayApp::MdGatewayApp(config::Settings cfg,
                           std::unique_ptr<messaging::IMdControlSource> control_source,
                           std::shared_ptr<messaging::MdPublisher> md_sink,
                           std::unique_ptr<messaging::IAckPublisher> ack_sink,
                           std::shared_ptr<messaging::IFundingRatePublisher> funding_sink,
                           const bpt::common::util::Topology& topology)
    : cfg_(std::move(cfg)),
      metrics_(cfg_.metrics_host, cfg_.base.metrics_port),
      md_pub_(std::move(md_sink)),
      funding_pub_(std::move(funding_sink)),
      ack_pub_(std::move(ack_sink)),
      ctrl_sub_(std::move(control_source)) {
    for (const auto& a_cfg : cfg_.adapters) {
        // Validate against ExchangeRegistry at the boundary so a typo in
        // the TOML fails immediately rather than skipping silently with a
        // warning. The registry is YAML-driven; new venues land here when
        // messages/exchanges.yaml is edited and codegen rerun.
        const auto exch_id = bpt::messages::ExchangeRegistry::from_name(a_cfg.exchange);
        if (!exch_id) {
            throw std::runtime_error(fmt::format(
                "Unknown exchange '{}' in mdgw config — not in messages/exchanges.yaml",
                a_cfg.exchange));
        }
        auto adapter = adapter::make_md_adapter<messaging::MdPublisher>(
            *exch_id, a_cfg, md_pub_);
        if (!adapter) {
            throw std::runtime_error(fmt::format(
                "Exchange '{}' is in the registry but mdgw has no adapter implementation for it",
                a_cfg.exchange));
        }

        adapter->on_funding_rate = [this](const messaging::FundingRateUpdate& fr) {
            funding_pub_->publish(fr);
        };

        // Use raw pointers into the metrics families — these are stable for the
        // lifetime of MdGatewayApp (metrics_ is a member).
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

    bpt::common::log::info("MdGateway ready — polling control stream {}", cfg_.aeron.control.stream_id);
}

void MdGatewayApp::run() {
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

        // Sync drop counter from MdPublisher into the metrics gauge.
        // Cheap relaxed load — done every idle iteration so lag is at most ~10µs.
        metrics_.md_messages_dropped->Set(static_cast<double>(md_pub_->drop_count()));

        // Snapshot decode latency histograms and push per-exchange gauges to Prometheus.
        if (now - last_lat_report >= lat_report_interval) {
            for (auto& [exchange, hist] : lat_reporters_)
                metrics_.update_decode_latency(exchange, *hist);
            for (auto& [exchange, a] : md_stat_reporters_) {
                metrics_.md_messages_published(exchange).Set(static_cast<double>(a->md_published_count()));
                metrics_.md_validation_drops(exchange).Set(static_cast<double>(a->validation_drop_count()));
                metrics_.validation_drop_breaker_tripped(exchange)
                    .Set(a->validation_drop_breaker_tripped() ? 1.0 : 0.0);
            }
            last_lat_report = now;
        }
    }
}

void MdGatewayApp::stop() {
    // Called by bpt::app::run() after our run() loop exits on signal.
    // Adapter WS threads + Prometheus exposer are drained here so the
    // startup-log side-effects remain symmetric with teardown.
    sub_mgr_.stop_all();
    metrics_.shutdown();
}

}  // namespace bpt::md_gateway

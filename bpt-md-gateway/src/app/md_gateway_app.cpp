#include "md_gateway/app/md_gateway_app.h"

#include "md_gateway/adapter/binance/binance_adapter.h"
#include "md_gateway/adapter/deribit/deribit_adapter.h"
#include "md_gateway/adapter/hyperliquid/hyperliquid_adapter.h"
#include "md_gateway/adapter/okx/okx_adapter.h"

#include <FragmentAssembler.h>

#include <messages/MdSubscribeBatch.h>
#include <messages/MessageHeader.h>

#include <chrono>
#include <thread>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/signal.h>

using namespace std::chrono_literals;

namespace bpt::md_gateway {

MdGatewayApp::MdGatewayApp(config::Settings cfg, std::shared_ptr<aeron::Aeron> aeron)
    : cfg_(std::move(cfg)),
      aeron_(aeron),
      metrics_(cfg_.metrics_host, cfg_.metrics_port),
      ack_pub_(aeron, cfg_.aeron.ack_hb.channel, cfg_.aeron.ack_hb.stream_id) {
    md_pub_ = std::make_shared<messaging::MdPublisher>(aeron, cfg_.aeron.data.channel, cfg_.aeron.data.stream_id);
    funding_pub_ = std::make_shared<messaging::FundingRatePublisher>(aeron,
                                                                     cfg_.aeron.funding_rate.channel,
                                                                     cfg_.aeron.funding_rate.stream_id);

    ctrl_sub_ = bpt::common::aeron::wait_for_subscription(aeron, cfg_.aeron.control.channel, cfg_.aeron.control.stream_id);

    for (const auto& a_cfg : cfg_.adapters) {
        std::shared_ptr<adapter::IAdapter> adapter;
        if (a_cfg.exchange == "BINANCE") {
            adapter = std::make_shared<adapter::BinanceAdapter>(a_cfg, md_pub_);
        } else if (a_cfg.exchange == "OKX") {
            adapter = std::make_shared<adapter::OkxAdapter>(a_cfg, md_pub_);
        } else if (a_cfg.exchange == "HYPERLIQUID") {
            adapter = std::make_shared<adapter::HyperliquidAdapter>(a_cfg, md_pub_);
        } else if (a_cfg.exchange == "DERIBIT") {
            adapter = std::make_shared<adapter::DeribitAdapter>(a_cfg, md_pub_);
        } else {
            bpt::common::log::warn("Unknown exchange in config: {}", a_cfg.exchange);
            continue;
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

    aeron::FragmentAssembler ctrl_assembler([this](aeron::AtomicBuffer& buf,
                                                   aeron::util::index_t offset,
                                                   aeron::util::index_t length,
                                                   aeron::Header& /*hdr*/) {
        using namespace bpt::messages;

        if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
            return;

        char* data = reinterpret_cast<char*>(buf.buffer()) + offset;
        MessageHeader hdr(data, static_cast<std::size_t>(length));

        if (hdr.templateId() != MdSubscribeBatch::sbeTemplateId())
            return;

        MdSubscribeBatch msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));

        bpt::common::log::info("Received MdSubscribeBatch correlation_id={}", msg.correlationId());
        sub_mgr_.apply_batch(msg, ack_pub_);
        metrics_.subscription_batches_total->Increment();
    });

    while (bpt::common::signal::is_running()) {
        int fragments = ctrl_sub_->poll(ctrl_assembler.handler(), 10);

        auto now = std::chrono::steady_clock::now();

        if (now - last_svc_hb >= svc_hb_interval) {
            ack_pub_.publish_service_heartbeat();
            metrics_.service_heartbeats_total->Increment();
            last_svc_hb = now;
        }

        if (now - last_sub_hb >= sub_hb_interval) {
            sub_mgr_.publish_subscription_heartbeats(ack_pub_);
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
            }
            last_lat_report = now;
        }
    }

    sub_mgr_.stop_all();
    metrics_.shutdown();
    bpt::common::log::info("MdGateway shutting down");
}

}  // namespace bpt::md_gateway

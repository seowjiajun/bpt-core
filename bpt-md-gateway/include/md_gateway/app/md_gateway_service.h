#pragma once

/// \file
/// \brief Top-level lifecycle owner for the `bpt-md-gateway` process.
///
/// MdGatewayService is the `bpt::app::IService` the process entry point
/// hands to `bpt::app::run()`. It owns:
///   - the loaded `Settings`,
///   - the Aeron-port objects (built by MdGatewayAeronBus::build in main),
///   - one SubscriptionManager that fans control batches out to the
///     per-venue adapters,
///   - the Prometheus registry/exposer,
///   - the periodic latency + per-adapter MD-stat reporters that
///     scrape histograms into gauges every ~5 s.
///
/// `run()` blocks on the main poll loop until `stop()` flips the
/// running flag. The poll loop services the control subscription, the
/// service-heartbeat timer, and the reporter timer; per-venue WS reads
/// run on adapter-owned IO threads, not in here.

#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/config/settings.h"
#include "md_gateway/messaging/publishers/api/ack_publisher.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"
#include "md_gateway/messaging/publishers/md_publisher.h"
#include "md_gateway/messaging/subscribers/api/md_control_subscriber.h"
#include "md_gateway/metrics/metrics.h"
#include "md_gateway/subscription/subscription_manager.h"

#include <bpt_app/app.h>
#include <bpt_common/util/latency_histogram.h>
#include <bpt_common/util/topology.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace bpt::md_gateway {

/// \brief Lifecycle service for the gateway.
///
/// Constructor takes everything pre-built — the bus ports, topology,
/// settings — so the app itself contains no Aeron / port construction
/// logic. That stays in main.cpp via `messaging::MdGatewayAeronBus::build`.
class MdGatewayService : public bpt::app::IService {
public:
    /// \brief Construct.
    /// \param cfg              loaded settings (ownership taken)
    /// \param control_sub   subscriber on the strategy → gateway control stream
    /// \param md_pub          publisher for normalised MD on the data stream
    /// \param ack_pub         publisher for acks + heartbeats
    /// \param funding_pub     publisher for funding-rate updates
    /// \param stats_pub          publisher for open-interest updates
    /// \param topology         CPU-affinity map for IO/main thread pinning
    MdGatewayService(config::Settings cfg,
                 std::unique_ptr<messaging::api::MdControlSubscriber> control_sub,
                 std::shared_ptr<messaging::MdPublisher> md_pub,
                 std::unique_ptr<messaging::api::AckPublisher> ack_pub,
                 std::shared_ptr<messaging::api::FundingRatePublisher> funding_pub,
                 std::shared_ptr<messaging::api::InstrumentStatsPublisher> stats_pub,
                 const bpt::common::util::Topology& topology);

    /// \brief Block running the main poll loop until stop() is called.
    void run() override;

    /// \brief Signal run() to exit. Safe to call from any thread (typically the signal handler).
    void stop() override;

private:
    config::Settings cfg_;
    metrics::MdGatewayMetrics metrics_;
    std::shared_ptr<messaging::MdPublisher> md_pub_;
    std::shared_ptr<messaging::api::FundingRatePublisher> funding_pub_;
    std::shared_ptr<messaging::api::InstrumentStatsPublisher> stats_pub_;
    std::unique_ptr<messaging::api::AckPublisher> ack_pub_;
    std::unique_ptr<messaging::api::MdControlSubscriber> ctrl_sub_;
    subscription::SubscriptionManager sub_mgr_;

    /// \name Reporter wiring
    /// \brief Collected at construction; consumed by the periodic latency + MD-stat reporter inside run().
    /// @{
    std::vector<std::pair<std::string, bpt::common::util::LatencyHistogram*>> lat_reporters_;
    std::vector<std::pair<std::string, adapter::IAdapter*>> md_stat_reporters_;
    /// @}
};

}  // namespace bpt::md_gateway

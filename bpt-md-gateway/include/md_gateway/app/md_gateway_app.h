#pragma once

#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/config/settings.h"
#include "md_gateway/messaging/ack_publisher.h"
#include "md_gateway/messaging/funding_rate_publisher.h"
#include "md_gateway/messaging/md_publisher.h"
#include "md_gateway/metrics/metrics.h"
#include "md_gateway/subscription/subscription_manager.h"

#include <Aeron.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <yggdrasil/util/latency_histogram.h>

namespace bpt::md_gateway {

class MdGatewayApp {
public:
    MdGatewayApp(config::Settings cfg, std::shared_ptr<aeron::Aeron> aeron);
    void run();

private:
    config::Settings cfg_;
    std::shared_ptr<aeron::Aeron> aeron_;
    metrics::MdGatewayMetrics metrics_;
    std::shared_ptr<messaging::MdPublisher> md_pub_;
    std::shared_ptr<messaging::FundingRatePublisher> funding_pub_;
    messaging::AckPublisher ack_pub_;
    std::shared_ptr<aeron::Subscription> ctrl_sub_;
    subscription::SubscriptionManager sub_mgr_;

    // Collected at construction; used by the periodic latency reporter in run().
    std::vector<std::pair<std::string, ygg::util::LatencyHistogram*>> lat_reporters_;
    std::vector<std::pair<std::string, adapter::IAdapter*>> md_stat_reporters_;
};

}  // namespace bpt::md_gateway

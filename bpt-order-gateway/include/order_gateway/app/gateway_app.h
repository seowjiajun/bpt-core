#pragma once

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/config/settings.h"
#include "order_gateway/messaging/i_account_snapshot_publisher.h"
#include "order_gateway/messaging/i_exec_report_publisher.h"
#include "order_gateway/messaging/i_heartbeat_publisher.h"
#include "order_gateway/messaging/i_order_control_source.h"
#include "order_gateway/metrics/metrics.h"
#include "order_gateway/order/order_processor.h"
#include "order_gateway/order/order_state_manager.h"
#include "order_gateway/risk/pnl_tracker.h"
#include "order_gateway/risk/risk_checker.h"

#include <map>
#include <memory>
#include <string>
#include <vector>
#include <bpt_app/app.h>
#include <bpt_common/util/topology.h>

namespace bpt::order_gateway {

class OrderGatewayApp : public bpt::app::IService {
public:
    OrderGatewayApp(config::Settings cfg,
                    std::shared_ptr<messaging::IOrderControlSource> control_source,
                    std::shared_ptr<messaging::IExecReportPublisher> exec_sink,
                    std::shared_ptr<messaging::IAccountSnapshotPublisher> account_snapshot_sink,
                    std::shared_ptr<messaging::IHeartbeatPublisher> heartbeat_sink,
                    std::map<std::string, adapter::ExchangeCredentials> creds,
                    const bpt::common::util::Topology& topology);
    void run() override;
    void stop() override;

private:
    config::Settings cfg_;
    metrics::OrderGatewayMetrics metrics_;
    std::shared_ptr<messaging::IExecReportPublisher> exec_pub_;
    std::shared_ptr<messaging::IAccountSnapshotPublisher> account_snap_pub_;
    std::shared_ptr<messaging::IHeartbeatPublisher> hb_pub_;
    std::shared_ptr<messaging::IOrderControlSource> order_sub_;
    risk::RiskChecker risk_checker_;
    risk::PnlTracker pnl_tracker_;
    order::OrderStateManager state_mgr_;
    std::vector<std::shared_ptr<adapter::IOrderAdapter>> adapters_;
    std::unique_ptr<order::OrderProcessor> processor_;
    const bpt::common::util::Topology& topology_;
};

}  // namespace bpt::order_gateway

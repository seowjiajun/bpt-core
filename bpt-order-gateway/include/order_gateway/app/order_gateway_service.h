#pragma once

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/app/account_snap_executor.h"
#include "order_gateway/config/settings.h"
#include "order_gateway/messaging/publishers/api/account_snapshot_publisher.h"
#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"
#include "order_gateway/messaging/publishers/api/heartbeat_publisher.h"
#include "order_gateway/messaging/subscribers/api/order_subscriber.h"
#include "order_gateway/metrics/metrics.h"
#include "order_gateway/order/order_processor.h"
#include "order_gateway/order/order_state_manager.h"
#include "order_gateway/risk/pnl_tracker.h"
#include "order_gateway/risk/pre_trade_risk_gate.h"
#include "order_gateway/risk/risk_checker.h"

#include <bpt_app/app.h>
#include <bpt_common/util/topology.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bpt::order_gateway {

class OrderGatewayService : public bpt::app::IService {
public:
    OrderGatewayService(config::Settings cfg,
                        std::shared_ptr<messaging::api::OrderSubscriber> control_sub,
                        std::shared_ptr<messaging::api::ExecReportPublisher> exec_pub,
                        std::shared_ptr<messaging::api::AccountSnapshotPublisher> account_snapshot_pub,
                        std::shared_ptr<messaging::api::HeartbeatPublisher> heartbeat_pub,
                        std::map<std::string, adapter::ExchangeCredentials> creds,
                        const bpt::common::util::Topology& topology);
    void run() override;
    void stop() override;

private:
    config::Settings cfg_;
    metrics::OrderGatewayMetrics metrics_;
    std::shared_ptr<messaging::api::ExecReportPublisher> exec_pub_;
    std::shared_ptr<messaging::api::AccountSnapshotPublisher> account_snap_pub_;
    std::shared_ptr<messaging::api::HeartbeatPublisher> hb_pub_;
    std::shared_ptr<messaging::api::OrderSubscriber> order_sub_;
    risk::RiskChecker risk_checker_;
    risk::PnlTracker pnl_tracker_;
    risk::PreTradeRiskGate risk_gate_;
    order::OrderStateManager state_mgr_;
    std::vector<std::shared_ptr<adapter::IOrderAdapter>> adapters_;
    std::unique_ptr<order::OrderProcessor> processor_;
    std::unique_ptr<app::AccountSnapExecutor> snap_executor_;
    const bpt::common::util::Topology& topology_;
};

}  // namespace bpt::order_gateway

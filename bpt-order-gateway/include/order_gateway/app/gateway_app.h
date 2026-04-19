#pragma once

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/i_order_adapter.h"
#include "order_gateway/config/settings.h"
#include "order_gateway/messaging/account_snapshot_publisher.h"
#include "order_gateway/messaging/exec_report_publisher.h"
#include "order_gateway/messaging/heartbeat_publisher.h"
#include "order_gateway/messaging/order_subscriber.h"
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

namespace bpt::order_gateway {

class OrderGatewayApp : public bpt::app::IService {
public:
    OrderGatewayApp(config::Settings cfg,
                std::shared_ptr<aeron::Aeron> aeron,
                std::map<std::string, adapter::ExchangeCredentials> creds);
    void run() override;
    void stop() override;

private:
    config::Settings cfg_;
    std::shared_ptr<aeron::Aeron> aeron_;
    metrics::OrderGatewayMetrics metrics_;
    std::shared_ptr<messaging::ExecReportPublisher> exec_pub_;
    std::shared_ptr<messaging::AccountSnapshotPublisher> account_snap_pub_;
    std::shared_ptr<messaging::HeartbeatPublisher> hb_pub_;
    std::shared_ptr<messaging::OrderSubscriber> order_sub_;
    risk::RiskChecker risk_checker_;
    risk::PnlTracker pnl_tracker_;
    order::OrderStateManager state_mgr_;
    std::vector<std::shared_ptr<adapter::IOrderAdapter>> adapters_;
    std::unique_ptr<order::OrderProcessor> processor_;
};

}  // namespace bpt::order_gateway

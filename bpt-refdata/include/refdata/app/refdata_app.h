#pragma once

#include "refdata/adapter/common/i_exchange_refdata_adapter.h"
#include "refdata/adapter/credentials.h"
#include "refdata/config/settings.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/mapping/instrument_mapping_merger.h"
#include "refdata/messaging/fee_schedule_publisher.h"
#include "refdata/messaging/refdata_status_publisher.h"
#include "refdata/messaging/refdata_control_subscriber.h"
#include "refdata/messaging/refdata_delta_publisher.h"
#include "refdata/messaging/refdata_snapshot_publisher.h"
#include "refdata/messaging/subscription_manager.h"
#include "refdata/metrics/metrics.h"
#include "refdata/registry/instrument_registry.h"

#include <Aeron.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bpt::refdata {

class RefdataApp {
public:
    RefdataApp(config::Settings settings,
              std::shared_ptr<aeron::Aeron> aeron,
              std::map<std::string, adapter::ExchangeCredentials> creds);
    void run();

private:
    config::Settings settings_;
    std::shared_ptr<aeron::Aeron> aeron_;
    metrics::RefdataMetrics metrics_;
    std::shared_ptr<mapping::InstrumentMappingLoader> instrument_mapping_;
    std::optional<mapping::InstrumentMappingMerger> mapping_merger_;
    std::shared_ptr<registry::InstrumentRegistry> registry_;
    std::unique_ptr<messaging::RefdataControlSubscriber> control_sub_;
    std::unique_ptr<messaging::RefdataSnapshotPublisher> snapshot_pub_;
    std::shared_ptr<messaging::RefdataDeltaPublisher> delta_pub_;
    std::shared_ptr<messaging::FeeSchedulePublisher> fee_pub_;
    std::shared_ptr<messaging::RefdataStatusPublisher> status_pub_;
    std::vector<std::unique_ptr<adapter::IExchangeRefDataAdapter>> adapters_;
    messaging::SubscriptionManager sub_manager_;
    std::mutex pub_mutex_;  // Guards publisher calls during parallel snapshot fetch
};

}  // namespace bpt::refdata

#pragma once

#include "refdata/adapter/common/i_exchange_refdata_adapter.h"
#include "refdata/adapter/credentials.h"
#include "refdata/config/settings.h"
#include "refdata/mapping/instrument_mapping_loader.h"
#include "refdata/mapping/instrument_mapping_merger.h"
#include "refdata/messaging/subscription_manager.h"
#include "refdata/metrics/metrics.h"
#include "refdata/port/i_fee_schedule_sink.h"
#include "refdata/port/i_refdata_control_source.h"
#include "refdata/port/i_refdata_delta_sink.h"
#include "refdata/port/i_refdata_snapshot_sink.h"
#include "refdata/port/i_refdata_status_sink.h"
#include "refdata/registry/instrument_registry.h"

#include <bpt_app/app.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bpt::refdata {

class RefdataService : public bpt::app::IService {
public:
    RefdataService(config::Settings settings,
               std::unique_ptr<port::IRefdataControlSource> control_source,
               std::unique_ptr<port::IRefdataSnapshotSink> snapshot_sink,
               std::shared_ptr<port::IRefdataDeltaSink> delta_sink,
               std::shared_ptr<port::IFeeScheduleSink> fee_sink,
               std::shared_ptr<port::IRefdataStatusSink> status_sink,
               std::map<std::string, adapter::ExchangeCredentials> creds);
    void run() override;
    void stop() override;

    /// \brief Handle a decoded subscription request. Public so unit tests
    ///        can drive the seam without spinning the run loop.
    void handle_request(const messaging::RefdataRequest& request);

private:
    config::Settings settings_;
    metrics::RefdataMetrics metrics_;
    std::shared_ptr<mapping::InstrumentMappingLoader> instrument_mapping_;
    std::optional<mapping::InstrumentMappingMerger> mapping_merger_;
    std::shared_ptr<registry::InstrumentRegistry> registry_;
    std::unique_ptr<port::IRefdataControlSource> control_sub_;
    std::unique_ptr<port::IRefdataSnapshotSink> snapshot_pub_;
    std::shared_ptr<port::IRefdataDeltaSink> delta_pub_;
    std::shared_ptr<port::IFeeScheduleSink> fee_pub_;
    std::shared_ptr<port::IRefdataStatusSink> status_pub_;
    std::vector<std::unique_ptr<adapter::IExchangeRefDataAdapter>> adapters_;
    messaging::SubscriptionManager sub_manager_;
    std::mutex pub_mutex_;  // Guards publisher calls during parallel snapshot fetch
};

}  // namespace bpt::refdata

#pragma once

/// @file
/// RecorderService — bpt-tape's IService implementation. Owns the per-venue
/// recording adapters (templated on NoopMdPublisher), the REST refdata
/// pollers, and the metrics exposer. main.cpp constructs one of these and
/// hands it to bpt::app::run; everything else lives here so the binary
/// entry point stays a thin wrapper and so RecorderService can be unit-
/// tested without booting the framework.
///
/// Lifetime: built once at startup, ::run() blocks until SIGINT, ::stop()
/// joins adapter threads + writes SESSION_STOP markers + flips
/// healthy=0. Member declaration order matters — TapeMetrics outlives
/// every spool because spools' metrics-hook lambdas capture refs into
/// its prometheus families.

#include "bpt_common/recorder/raw_spool.h"
#include "md_gateway/adapter/common/i_adapter.h"
#include "tape/config/settings.h"
#include "tape/metrics/metrics.h"
#include "tape/refdata/refdata_poller.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <bpt_app/app.h>
#include <bpt_common/util/topology.h>

namespace bpt::tape::app {

class RecorderService : public bpt::app::IService {
public:
    RecorderService(config::Settings settings,
                    const bpt::common::util::Topology& topology);

    void run() override;
    void stop() override;

private:
    /// First-connect-vs-reconnect tracking for SESSION_START / WS_RECONNECT
    /// markers. One per spool, captured by both on_connect and on_disconnect.
    struct ConnState {
        bool was_disconnected{false};
        uint32_t reconnect_count{0};
    };

    /// Constructor phases — kept as named methods so the ctor body is a
    /// 4-line orchestrator and each phase is independently navigable.
    void setup_metrics();
    void setup_mdgw_recording();
    void setup_universe();
    void setup_refdata_pollers();

    config::Settings settings_;
    const bpt::common::util::Topology& topology_;

    // Built before any spool; destroyed after all of them. The hook
    // lambdas inside each spool capture refs into TapeMetrics-owned
    // prometheus families, so the declaration order (and thus reverse
    // destruction order) is load-bearing.
    std::unique_ptr<metrics::TapeMetrics> metrics_;

    std::vector<std::shared_ptr<bpt::common::recorder::RawSpool>> spools_;
    std::vector<std::shared_ptr<bpt::md_gateway::adapter::IAdapter>> adapters_;
    std::unordered_map<std::string,
                       std::shared_ptr<bpt::md_gateway::adapter::IAdapter>>
        adapters_per_venue_;

    std::vector<std::shared_ptr<bpt::common::recorder::RawSpool>> refdata_spools_;
    std::vector<std::unique_ptr<refdata::RefdataPoller>> refdata_pollers_;
};

}  // namespace bpt::tape::app

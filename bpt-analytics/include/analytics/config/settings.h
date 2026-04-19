#pragma once

#include <cstdint>
#include <string>
#include <bpt_common/aeron/stream_config.h>

namespace bpt::analytics::config {

struct LoggingConfig {
    std::string level{"info"};
    std::string dir{"logs"};
};

struct Settings {
    std::string media_driver_dir;

    // Inputs (read-only subscriptions)
    bpt::common::config::StreamConfig exec_report;   // order-gateway → stream 3002
    bpt::common::config::StreamConfig md_data;       // bpt-md-gateway → stream 2002

    // Output
    bpt::common::config::StreamConfig toxicity;      // tyr → stream 5001

    // Analysis parameters
    std::size_t markout_max_pending{64};
    std::size_t scorer_window_size{50};
    uint64_t scorer_window_duration_ns{0};   // 0 = size-only
    std::size_t scorer_min_samples{5};
    uint32_t publish_interval_ms{5000};      // how often to publish ToxicityUpdate

    LoggingConfig logging;
    uint16_t metrics_port{9105};
};

Settings load(const std::string& path);

}  // namespace bpt::analytics::config

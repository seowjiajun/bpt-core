#pragma once

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <string>

namespace bpt::analytics::config {

struct Settings {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    // Inputs (read-only subscriptions)
    bpt::common::config::StreamConfig exec_report;  // order-gateway → stream 3002
    bpt::common::config::StreamConfig md_data;      // bpt-md-gateway → stream 2002

    // Output
    bpt::common::config::StreamConfig toxicity;  // tyr → stream 5001

    // Analysis parameters
    std::size_t markout_max_pending{64};
    std::size_t scorer_window_size{50};
    uint64_t scorer_window_duration_ns{0};  // 0 = size-only
    std::size_t scorer_min_samples{5};
    uint32_t publish_interval_ms{5000};  // how often to publish ToxicityUpdate
};

Settings load(const std::string& path);

}  // namespace bpt::analytics::config

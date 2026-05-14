#pragma once

/// \file
/// \brief Loaded configuration for `bpt-radar`.

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <string>

namespace bpt::radar::config {

struct Settings {
    /// Shared lifecycle config (env, media_driver_dir, logging, metrics_port, calibrate_tsc).
    bpt::app::BaseSettings base;

    // Inputs (read-only subscriptions)
    bpt::common::config::StreamConfig vol_surface;       ///< bpt-pricer → stream 4001
    bpt::common::config::StreamConfig instrument_stats;  ///< bpt-md-gateway → stream 2004

    // Output
    bpt::common::config::StreamConfig market_color;  ///< bpt-radar → stream 6002

    /// How often to recompute + publish MarketColor (one frame per
    /// exchange/underlying tuple per interval). Default mirrors bpt-pricer's
    /// surface-publish cadence — pointless to compute faster than the input
    /// updates.
    uint32_t publish_interval_ms{2000};
};

Settings load(const std::string& path);

}  // namespace bpt::radar::config

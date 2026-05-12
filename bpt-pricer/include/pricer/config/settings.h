#pragma once

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <string>
#include <vector>

namespace bpt::pricer::config {

struct Settings {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    // MD input (reads from MdGateway stream 2002 = md_data)
    bpt::common::config::StreamConfig md_data;

    // Refdata input (reads from Sindri streams 1001-1003)
    bpt::common::config::StreamConfig refdata_snapshot;
    bpt::common::config::StreamConfig refdata_delta;
    bpt::common::config::StreamConfig refdata_control;  // Strategy → Sindri (we reuse for our subscription)

    // Vol surface output (Pricer → Strategy stream 4001)
    bpt::common::config::StreamConfig vol_surface;

    // Heartbeat + ready output (Pricer → Strategy stream 4002 = pricer_status)
    bpt::common::config::StreamConfig pricer_status;

    // Exchanges to track options on
    std::vector<std::string> exchanges;

    // Underlyings to compute surfaces for (e.g. "BTC", "ETH")
    std::vector<std::string> underlyings;

    // How often (ms) to recompute and publish the full surface
    uint32_t publish_interval_ms{2000};

    // Risk-free rate for Black-Scholes (annualised)
    double risk_free_rate{0.05};

    // IV solver settings
    uint32_t newton_max_iterations{100};
    double newton_tolerance{1e-8};
};

Settings load(const std::string& path);

}  // namespace bpt::pricer::config

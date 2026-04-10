#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <yggdrasil/aeron/stream_config.h>

namespace surtr::config {

struct LoggingConfig {
    std::string level{"info"};  // trace | debug | info | warn | error | critical | off
    std::string dir{"logs"};
};

struct Settings {
    std::string media_driver_dir;

    // MD input (reads from Huginn stream 2002)
    ygg::config::StreamConfig md_input;

    // Refdata input (reads from Sindri streams 1001-1002)
    ygg::config::StreamConfig refdata_snapshot;
    ygg::config::StreamConfig refdata_delta;
    ygg::config::StreamConfig refdata_control;  // Fenrir → Sindri (we reuse for our subscription)

    // Vol surface output (Surtr → Fenrir stream 4001)
    ygg::config::StreamConfig vol_surface;

    // Status output (Surtr → Fenrir stream 4002)
    ygg::config::StreamConfig status;

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

    // Aeron pub registration
    int pub_timeout_ms{5000};
    int pub_poll_interval_ms{10};

    LoggingConfig logging;
    uint16_t metrics_port{9103};
};

Settings load(const std::string& path);

}  // namespace surtr::config

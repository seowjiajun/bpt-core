#pragma once

#include <cstdint>
#include <string>
#include <yggdrasil/aeron/stream_config.h>
#include <yggdrasil/logging.h>

namespace bridge::config {

struct Settings {
    // Aeron
    std::string             media_driver_dir{"/dev/shm/aeron-bifrost"};
    ygg::config::StreamConfig md_data{"aeron:ipc", 2002};       // Huginn → everyone
    ygg::config::StreamConfig exec_report{"aeron:ipc", 3002};   // Heimdall → Fenrir/dashboard

    // WebSocket
    uint16_t ws_port{8080};

    // Session
    std::string symbol{"BTC-USDT"};
    std::string strategy{"unknown"};    // display only; set via config or --strategy-name
    std::string exchange{"OKX"};        // display only
    double      starting_capital{100'000.0};

    // Logging
    ygg::logging::LogConfig logging;
};

Settings load(const std::string& path);

}  // namespace bridge::config

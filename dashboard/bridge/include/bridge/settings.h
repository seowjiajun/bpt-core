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
    ygg::config::StreamConfig control_command{"aeron:ipc", 9003}; // bridge → Fenrir (halt/resume)

    // WebSocket
    uint16_t ws_port{8080};

    // Session
    std::string symbol{"BTC-USDT"};
    std::string strategy{"unknown"};    // display only; set via config or --strategy-name
    std::string exchange{"OKX"};        // display only
    std::string mode{"backtest"};       // display only; "backtest" | "paper" | "live"
    double      starting_capital{100'000.0};

    // Instrument filter: when non-zero, the bridge drops MD ticks and fills
    // that aren't for this instrument_id.  When zero, everything is forwarded
    // (single-instrument runs work without configuring the filter, but
    // multi-instrument runs will mix instruments on the dashboard).
    uint64_t    instrument_id{0};

    // Logging
    ygg::logging::LogConfig logging;
};

Settings load(const std::string& path);

}  // namespace bridge::config

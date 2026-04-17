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
    ygg::config::StreamConfig exec_report{"aeron:ipc", 3002};   // Heimdall → Strategy/dashboard
    ygg::config::StreamConfig control_command{"aeron:ipc", 9003}; // bridge → Strategy (halt/resume)
    ygg::config::StreamConfig portfolio_snapshot{"aeron:ipc", 9004}; // Strategy → bridge (portfolio state)
    ygg::config::StreamConfig account_snapshot{"aeron:ipc", 3004};   // Heimdall → bridge (live balance)
    ygg::config::StreamConfig toxicity{"aeron:ipc", 0};             // Analytics → bridge (ToxicityUpdate)

    // WebSocket
    uint16_t ws_port{8080};

    // Session
    std::string symbol{"BTC-USDT"};
    std::string strategy{"unknown"};    // display only; set via config or --strategy-name
    std::string exchange{"OKX"};        // display only
    std::string mode{"paper"};          // display only; "paper" | "live"
    std::string instrument_type{"SPOT"}; // display only; "SPOT" | "PERP" | "FUTURE" | "OPTION"

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

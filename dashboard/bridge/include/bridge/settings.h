#pragma once

#include <cstdint>
#include <string>
#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/logging.h>

namespace bridge::config {

struct Settings {
    // Aeron
    std::string             media_driver_dir{"/dev/shm/aeron-bifrost"};
    bpt::common::config::StreamConfig md_data{"aeron:ipc", 2002};       // MdGateway → everyone
    bpt::common::config::StreamConfig exec_report{"aeron:ipc", 3002};   // OrderGateway → Strategy/dashboard
    bpt::common::config::StreamConfig control_command{"aeron:ipc", 9003}; // bridge → Strategy (halt/resume)
    bpt::common::config::StreamConfig portfolio_snapshot{"aeron:ipc", 9004}; // Strategy → bridge (portfolio state)
    bpt::common::config::StreamConfig account_snapshot{"aeron:ipc", 3004};   // OrderGateway → bridge (live balance)
    bpt::common::config::StreamConfig toxicity{"aeron:ipc", 0};             // Analytics → bridge (ToxicityUpdate)

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
    bpt::common::logging::LogConfig logging;
};

Settings load(const std::string& path);

}  // namespace bridge::config

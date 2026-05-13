#pragma once

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <string>

namespace bridge::config {

struct Settings {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    // Aeron stream IDs (channel lives on each StreamConfig; media driver
    // dir lives in base.media_driver_dir).
    // Field names match the global vocabulary in deploy/config/aeron/streams.toml —
    // a typo fails at load rather than silently wiring the wrong stream.
    bpt::common::config::StreamConfig md_data{"aeron:ipc", 2002};            // MdGateway → everyone
    bpt::common::config::StreamConfig exec_report{"aeron:ipc", 3002};        // OrderGateway → Strategy/bridge
    bpt::common::config::StreamConfig dashboard_control{"aeron:ipc", 9003};  // bridge → Strategy (halt/resume)
    bpt::common::config::StreamConfig portfolio{"aeron:ipc", 9004};          // Strategy → bridge (portfolio state)
    bpt::common::config::StreamConfig account_snapshot{"aeron:ipc", 3004};   // OrderGateway → bridge (live balance)
    bpt::common::config::StreamConfig toxicity{"aeron:ipc", 0};              // Analytics → bridge (ToxicityUpdate)

    // WebSocket
    uint16_t ws_port{8080};

    // Session
    std::string symbol{"BTC-USDT"};
    std::string strategy{"unknown"};      // display only; set via config or --strategy-name
    std::string exchange{"OKX"};          // display only
    std::string mode{"paper"};            // display only; "paper" | "live"
    std::string instrument_type{"SPOT"};  // display only; "SPOT" | "PERP" | "FUTURE" | "OPTION"

    // Instrument filter: when non-zero, the bridge drops MD ticks and fills
    // that aren't for this instrument_id.  When zero, everything is forwarded
    // (single-instrument runs work without configuring the filter, but
    // multi-instrument runs will mix instruments on the dashboard).
    uint64_t instrument_id{0};
};

/// \brief Load bridge settings from `path`.
///
/// `profile_override`, when non-empty, supersedes any `profile_config`
/// in the TOML. Use it from the systemd unit (`--profile`) so bridge
/// labels its env from the same deployment profile as the rest of the
/// stack instead of the hardcoded value baked into `bridge.live.toml`.
Settings load(const std::string& path, const std::string& profile_override = "");

}  // namespace bridge::config

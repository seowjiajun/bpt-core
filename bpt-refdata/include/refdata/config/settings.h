#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <yggdrasil/aeron/stream_config.h>
#include <yggdrasil/logging.h>

namespace bpt::refdata::config {

// Per-exchange REST + WebSocket endpoint configuration.
// Mirrors the pattern used by Huginn and Heimdall adapters.
struct AdapterConfig {
    std::string exchange;     // BINANCE | OKX | HYPERLIQUID
    std::string secret_name;  // AWS Secrets Manager secret name, e.g. "bpt/testnet/OKX"
    bool enabled{true};       // false = adapter is declared but not initialised at startup
    bool simulated{false};    // OKX demo trading — injects x-simulated-trading: 1
    std::string rest_host;
    std::string rest_port{"443"};
    std::string ws_host;
    std::string ws_port{"443"};
    bool use_tls{true};
};

struct InstrumentMappingConfig {
    std::string local_path{"/opt/bpt/data/instrument_mapping.json"};

    // S3 source — if bucket is non-empty, Refdata fetches the per-exchange mapping
    // files from S3 at startup (and daily thereafter), merges them in memory, and
    // writes the merged result atomically to local_path.
    // If bucket is empty, the file must already exist at local_path.
    //
    // keys: exchange_name (lowercase) → S3 object key
    //   e.g. { "okx" → "instrument-mapping/current/okx.json" }
    // Only the exchanges listed in keys are fetched — omit exchanges Refdata doesn't need.
    struct S3 {
        std::string bucket;
        std::string region{"ap-southeast-1"};
        std::map<std::string, std::string> keys;  // exchange_name → s3_key
    } s3;
};

struct Settings {
    std::string environment;             // "prod" | "qa" | "dev" — logged at startup, validated against exchange_config
    std::vector<std::string> exchanges;  // exchanges to activate from exchange_config (e.g. ["OKX", "BINANCE"])
    std::string media_driver_dir;        // Path to Aeron driver dir

    // Instrument refdata (existing streams 1001-1003)
    ygg::config::StreamConfig snapshot;
    ygg::config::StreamConfig delta;
    ygg::config::StreamConfig control;

    // Exchange-sourced refdata streams (1004, 1006)
    // Note: stream 1005 (FundingRate) has moved to Huginn
    ygg::config::StreamConfig fee_schedule;
    ygg::config::StreamConfig refdata_status;

    // Only exchange adapters listed here will be initialised.
    // Any runtime request for an unlisted exchange returns EXCHANGE_NOT_CONFIGURED.
    std::vector<AdapterConfig> adapters;

    // How often (seconds) to re-poll REST instrument listings for new/delisted instruments.
    // Instrument listings have no WebSocket delta feed on any exchange.
    uint32_t instrument_poll_interval_s{3600};

    InstrumentMappingConfig instrument_mapping;
    ygg::logging::LogConfig logging;

    uint16_t metrics_port{9101};
};

Settings load(const std::string& path);

}  // namespace bpt::refdata::config

#pragma once

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace bpt::refdata::config {

// Per-exchange REST + WebSocket endpoint configuration.
// Mirrors the pattern used by MdGateway and OrderGateway adapters.
struct AdapterConfig {
    std::string exchange;  // BINANCE | OKX | HYPERLIQUID
    std::string
        secret_name;        // systemd-creds name (slashes normalized to '-'); e.g. "bpt/testnet/OKX" → bpt-testnet-OKX
    bool enabled{true};     // false = adapter is declared but not initialised at startup
    bool simulated{false};  // OKX demo trading — injects x-simulated-trading: 1
    std::string rest_host;
    std::string rest_port{"443"};
    std::string ws_host;
    std::string ws_port{"443"};
    bool use_tls{true};

    // Optional TLS-pinning allowlist for REST + WS connections (see
    // bpt::common::ws::ws_connect). Lowercase hex SHA-256 leaf-cert
    // fingerprints, 64 chars each. Empty = no pinning.
    std::vector<std::string> pinned_tls_sha256;
};

struct InstrumentMappingConfig {
    std::string local_path{"/opt/bpt/data/instrument_mapping.json"};

    // Per-exchange source files produced by the data-forge pipeline.
    // Refdata merges them at startup and writes the result atomically to
    // local_path. If sources is empty, local_path must already exist.
    //
    // Keys: exchange_name (lowercase). Values: file paths, typically
    // "config/generated/instrument_mapping.<exchange>.json" shipped with the
    // deploy tarball. Only exchanges Refdata needs go here.
    struct Sources {
        std::map<std::string, std::string> paths;
    } sources;
};

struct Settings {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    std::vector<std::string> exchanges;  // exchanges to activate from exchange_config (e.g. ["OKX", "BINANCE"])

    // Instrument refdata (existing streams 1001-1003)
    bpt::common::config::StreamConfig snapshot;
    bpt::common::config::StreamConfig delta;
    bpt::common::config::StreamConfig control;

    // Exchange-sourced refdata streams (1004, 1006)
    // Note: stream 1005 (FundingRate) has moved to MdGateway
    bpt::common::config::StreamConfig fee_schedule;
    bpt::common::config::StreamConfig refdata_status;

    // Only exchange adapters listed here will be initialised.
    // Any runtime request for an unlisted exchange returns EXCHANGE_NOT_CONFIGURED.
    std::vector<AdapterConfig> adapters;

    // How often (seconds) to re-poll REST instrument listings for new/delisted instruments.
    // Instrument listings have no WebSocket delta feed on any exchange.
    uint32_t instrument_poll_interval_s{3600};

    InstrumentMappingConfig instrument_mapping;
};

Settings load(const std::string& path);

}  // namespace bpt::refdata::config

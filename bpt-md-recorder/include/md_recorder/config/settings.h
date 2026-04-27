#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <md_gateway/config/settings.h>

namespace bpt::md_recorder::config {

// Per-venue capture spec — what symbols to subscribe + at what depth.
// instrument_id is canonical, sourced from instrument_mapping.{venue}.json
// at startup and stamped onto SBE messages (downstream is no-op-published
// but adapters still parse + decode internally).
struct UniverseEntry {
    uint64_t instrument_id{0};
    std::string venue;       // "HYPERLIQUID", "OKX", "DERIBIT", "BINANCE"
    std::string symbol;      // exchange-native (HL coin, OKX instId, Deribit instrument)
    uint8_t depth{0};
};

// Recording knobs — one set per spool flavour. Today only mdgw recording
// is wired; refdata recording is a follow-up.
struct RecordingConfig {
    std::string output_dir{"/opt/bpt/data/raw"};
    uint32_t rotate_interval_seconds{3600};
    uint32_t fsync_interval_ms{1000};
    uint32_t buffer_bytes{1u << 20};
};

struct Settings {
    bpt::app::BaseSettings base;

    // Reuses bpt-md-gateway's per-venue WS adapter config (host, ports,
    // ping cadence, validation knobs). The recorder constructs the same
    // adapter classes with these configs, just wrapped in recording
    // subclasses that tee bytes to disk.
    std::vector<bpt::md_gateway::config::AdapterConfig> mdgw_adapters;

    // Recording universe — flat list of (venue, symbol, instrument_id, depth)
    // declared in the TOML. Future enhancement: derive from
    // instrument_mapping.{venue}.json with a tag filter; today the operator
    // hand-curates per recording corpus.
    std::vector<UniverseEntry> universe;

    // Per-venue allowlist applied against the adapter list — drops adapters
    // whose exchange isn't in this set. Empty = accept all.
    std::vector<std::string> recording_universe_venues;

    RecordingConfig recording;

    // Prometheus metrics bind host. Port comes from base.metrics_port.
    std::string metrics_host{"127.0.0.1"};
};

Settings load(const std::string& path);

}  // namespace bpt::md_recorder::config

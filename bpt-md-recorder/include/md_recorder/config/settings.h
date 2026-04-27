#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <md_gateway/config/settings.h>

namespace bpt::md_recorder::config {

// Universe filter — what to record from the instrument-mapping JSON.
// Operator declares high-level criteria; md-recorder iterates the loaded
// mapping for each venue, applies these filters, and calls subscribe()
// per surviving entry. Replaces the per-symbol [[universe]] hand-curation.
struct UniverseFilter {
    // Instrument types to include (empty = accept all). Matches
    // InstrumentInfo::type, e.g. "PERP", "SPOT", "FUTURE".
    std::vector<std::string> inst_types;
    // Symbols (canonical base) to drop even if they pass the type filter.
    // E.g. exclude BTC because spread is too tight for AS to clear fees.
    std::vector<std::string> exclude_bases;
    // Default subscription depth applied to every entry that survives
    // the filters. 0 = top-of-book bbo only; 5 = depth-5 ladder.
    uint8_t default_depth{5};
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

    // Path to the canonical instrument_mapping.json (the same file
    // bpt-refdata reads on the trading host). md-recorder loads this
    // at boot, iterates by venue, applies UniverseFilter, calls
    // subscribe() per surviving entry. Hot-reloadable on a periodic
    // tick if the operator wants new listings to land mid-session.
    std::string instrument_mapping_path{"config/instruments/instrument_mapping.json"};

    // Universe filter applied against the loaded mapping.
    UniverseFilter universe_filter;

    // Per-venue allowlist applied against the adapter list — drops adapters
    // whose exchange isn't in this set. Empty = accept all.
    std::vector<std::string> recording_universe_venues;

    RecordingConfig recording;

    // Prometheus metrics bind host. Port comes from base.metrics_port.
    std::string metrics_host{"127.0.0.1"};
};

Settings load(const std::string& path);

}  // namespace bpt::md_recorder::config

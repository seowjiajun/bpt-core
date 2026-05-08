#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <md_gateway/config/settings.h>

namespace bpt::tape::config {

// Universe filter — what to record from the instrument-mapping JSON.
// Operator declares high-level criteria; bpt-tape iterates the loaded
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

// Recording knobs — one set per spool flavour. WS frames go under
// {output_dir}/{venue}/...; refdata REST bodies under
// {output_dir}/{venue}-rest/... — same RawSpool format, distinct path so
// the WS converter doesn't have to filter them out.
struct RecordingConfig {
    std::string output_dir{"/opt/bpt/data/raw"};
    uint32_t rotate_interval_seconds{3600};
    uint32_t fsync_interval_ms{1000};
    uint32_t buffer_bytes{1u << 20};
};

// One configured REST endpoint to poll. bpt-tape pulls these on a timer
// and tees the response bodies to a `{venue}-rest` spool. Operator
// declares the URL shape — bpt-tape doesn't introspect bpt-refdata's
// adapter code, on purpose: the refdata service can grow new endpoints
// without changing the recorder, and vice versa.
struct RefdataEndpoint {
    std::string exchange;            ///< "HYPERLIQUID" — picks the spool
    std::string host;                ///< e.g. "api.hyperliquid.xyz"
    std::string port{"443"};
    bool use_tls{true};
    std::string method{"GET"};       ///< "GET" | "POST"
    std::string path;                ///< e.g. "/info"
    std::string body;                ///< POST body; ignored for GET
    uint32_t interval_seconds{3600}; ///< per-endpoint poll cadence
};

struct Settings {
    bpt::app::BaseSettings base;

    // Reuses bpt-md-gateway's per-venue WS adapter config (host, ports,
    // ping cadence, validation knobs). The recorder constructs the same
    // adapter classes with these configs, just wrapped in recording
    // subclasses that tee bytes to disk.
    std::vector<bpt::md_gateway::config::AdapterConfig> mdgw_adapters;

    // Path to the canonical instrument_mapping.json (the same file
    // bpt-refdata reads on the trading host). bpt-tape loads this
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

    // Operator-declared REST endpoints to poll + capture. Empty = refdata
    // recording disabled (the existing WS-only deploy).
    std::vector<RefdataEndpoint> refdata_endpoints;

    // Prometheus metrics bind host. Port comes from base.metrics_port.
    std::string metrics_host{"127.0.0.1"};
};

Settings load(const std::string& path);

}  // namespace bpt::tape::config

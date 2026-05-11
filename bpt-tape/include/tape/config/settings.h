#pragma once

/// \file
/// \brief Strongly-typed view of the bpt-tape TOML config.
///
/// Mirrors the structure of the .hl.toml file the operator edits. The
/// loader in config/loader.cpp populates these structs and surfaces any
/// validation errors at boot — runtime code reads only the populated
/// structs and never re-parses TOML.

#include <cstdint>
#include <string>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <md_gateway/config/settings.h>

namespace bpt::tape::config {

/// \brief Operator-declared filter over the instrument-mapping universe.
///
/// At boot bpt-tape walks the canonical mapping for each venue,
/// applies these filters, and subscribes to every survivor. Replaces
/// the per-symbol hand-curation that the earlier `[[universe]]`
/// TOML shape required.
struct UniverseFilter {
    /// Instrument types to include (empty = accept all). Matches
    /// `InstrumentInfo::type`, e.g. "PERP", "SPOT", "FUTURE".
    std::vector<std::string> inst_types;

    /// Canonical bases to drop even if they pass the type filter
    /// (e.g. "BTC" when its spread is too tight for the strategy in use).
    std::vector<std::string> exclude_bases;

    /// Subscription depth applied per surviving entry.
    /// 0 = top-of-book BBO only, 5 = depth-5 ladder.
    uint8_t default_depth{5};
};

/// \brief Recording-side knobs shared between WS and REST spools.
///
/// WS frames land under `{output_dir}/{venue}/...`, REST bodies under
/// `{output_dir}/{venue}-rest/...` — same wslog format, distinct paths
/// so the WS converter never sees REST records.
struct RecordingConfig {
    std::string output_dir{"/opt/bpt/data/raw"};
    uint32_t rotate_interval_seconds{3600};
    uint32_t fsync_interval_ms{1000};
    uint32_t buffer_bytes{1u << 20};
};

/// \brief A single REST endpoint to poll + record.
///
/// Operator declares the URL shape directly; bpt-tape doesn't
/// introspect bpt-refdata's adapter code so the refdata service can
/// grow new endpoints without recorder changes (and vice versa).
struct RefdataEndpoint {
    std::string exchange;            ///< venue tag, picks the spool
    std::string host;                ///< e.g. "api.hyperliquid.xyz"
    std::string port{"443"};
    bool use_tls{true};
    std::string method{"GET"};       ///< "GET" or "POST"
    std::string path;                ///< e.g. "/info"
    std::string body;                ///< POST body; ignored for GET
    uint32_t interval_seconds{3600}; ///< per-endpoint poll cadence
};

/// \brief Top-level bpt-tape settings, populated by load().
struct Settings {
    bpt::app::BaseSettings base;

    /// Per-venue WS adapter config — reuses bpt-md-gateway's shape so
    /// the recorder can construct the same adapter classes (just
    /// wrapped in Recording* subclasses that tee bytes to disk).
    std::vector<bpt::md_gateway::config::AdapterConfig> mdgw_adapters;

    /// Path to the canonical instrument-mapping JSON. Same file
    /// bpt-refdata reads on the trading host; bpt-tape walks it at
    /// boot, applies universe_filter, and subscribes per survivor.
    std::string instrument_mapping_path{"config/instruments/instrument_mapping.json"};

    /// Filter applied against the loaded mapping.
    UniverseFilter universe_filter;

    /// Per-venue allowlist — drops adapters whose exchange isn't in
    /// the set. Empty = accept all configured adapters.
    std::vector<std::string> recording_universe_venues;

    RecordingConfig recording;

    /// REST endpoints to poll + capture. Empty disables refdata
    /// recording (the WS-only deploy shape).
    std::vector<RefdataEndpoint> refdata_endpoints;

    /// Prometheus metrics bind host. Port comes from base.metrics_port.
    /// 0.0.0.0 in prod (in-VPC scraping); 127.0.0.1 in dev (SSH tunnel).
    std::string metrics_host{"127.0.0.1"};
};

/// \brief Parse `path` as TOML and return a populated Settings.
/// \throws std::runtime_error on missing required fields or malformed
///         TOML — the loader is strict at the config boundary so
///         silent misconfiguration can't reach runtime.
Settings load(const std::string& path);

}  // namespace bpt::tape::config

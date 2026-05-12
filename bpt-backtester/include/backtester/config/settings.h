#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>

namespace bpt::backtester::config {

// One entry per instrument Backtester will replay.
// exchange + symbol must exactly match the exchange-native symbol in the Parquet files.
struct InstrumentConfig {
    std::string exchange;  // BINANCE | OKX | HYPERLIQUID | DERIBIT
    std::string symbol;    // Exchange-native symbol e.g. BTCUSDT, BTC-USDT-SWAP, BTC
};

// Localhost WS ports that MdGateway and OrderGateway connect to instead of real exchanges.
struct EndpointConfig {
    uint16_t binance_md_port{9100};
    uint16_t okx_md_port{9101};
    uint16_t hyperliquid_md_port{9102};
    uint16_t deribit_md_port{9103};

    uint16_t binance_order_port{9110};
    uint16_t okx_order_port{9111};
    uint16_t hyperliquid_order_port{9112};
    uint16_t deribit_order_port{9113};

    // HL REST /info simulator (used by bpt-refdata + bpt-order-gateway in
    // backtest mode for instrument metadata + asset_idx mapping).
    uint16_t hyperliquid_info_port{9114};
};

// Per-venue, per-leg simulated latency. base_ns is the floor; jitter_ns
// adds a uniform [0, jitter_ns) draw on top, seeded once per run for
// reproducibility (see SimLatencyConfig::seed).
struct VenueLatencySpec {
    uint64_t submit_to_match_base_ns{0};
    uint64_t submit_to_match_jitter_ns{0};
    uint64_t match_to_report_base_ns{0};
    uint64_t match_to_report_jitter_ns{0};
};

struct SimLatencyConfig {
    // Per-venue overrides keyed by exchange string (BINANCE, OKX, HYPERLIQUID,
    // DERIBIT). Venues with no entry fall back to default_spec.
    std::unordered_map<std::string, VenueLatencySpec> per_venue;
    VenueLatencySpec default_spec;

    // Single seed for all latency draws. A fixed value (e.g., 1) gives
    // run-to-run reproducibility; rotate it to generate independent
    // realisations for sweep ensembles.
    uint64_t seed{1};
};

// One half-open replay window. Multiple windows can be unioned via
// [[simulation.windows]] — useful for "every Asian open", "all FOMC ±30s",
// or stitched walk-forward slices.
struct TimeWindow {
    std::string start;  // ISO8601: "YYYY-MM-DD" | "...THH:MM:SSZ" | "...THH:MM:SS.fffffffffZ"
    std::string end;
};

struct SimulationConfig {
    // Canonical list of replay windows. Always non-empty after config load.
    // Single-window configs (top-level start/end) auto-populate this with one
    // entry. The DataLoader emits events whose ts falls in ∪ windows.
    std::vector<TimeWindow> windows;

    // Back-compat scalar view. Pre-multi-window callers (logging, results-dir
    // tagging, summary.json metadata) read these. After load they hold the
    // span of the windows: start = front().start, end = back().end (windows
    // are sorted by start in the loader).
    std::string start;
    std::string end;

    bool allow_partial_data{false};
    uint32_t subscriber_wait_timeout_s{60};  // Wait up to N seconds for MdGateway to connect
    SimLatencyConfig latency;
};

struct DataConfig {
    std::string local_cache{"/opt/bpt/data/backtest-cache"};
    // Path to refdata snapshot JSON captured by bpt-refdata's recording mode.
    // Empty = HL info server unavailable (refdata in backtest mode will fail
    // to load instruments — set this to feed it from a recording).
    std::string hyperliquid_refdata_snapshot;
};

struct ResultsConfig {
    std::string output_dir{"results"};
    double starting_capital{100'000.0};

    // Per-venue maker/taker fee rates in basis points of notional. Looked
    // up by FillReport.exchange + FillReport.liquidity_role, so a single
    // run with mixed-venue fills bills each at its own rate.
    //
    // Defaults are 0.0 — TOML must set them explicitly so a missing entry
    // is loud at parse time rather than silently mispricing fills. The
    // venue key matches FillReport.exchange (e.g. "OKX", "HYPERLIQUID",
    // "BINANCE", "DERIBIT"). HL pays a maker rebate (negative bps);
    // CEX makers typically pay a small positive rate.
    struct FeeRates {
        double maker_bps{0.0};
        double taker_bps{0.0};
    };
    std::unordered_map<std::string, FeeRates> fees_by_venue;
    // Run identity — populated from CLI flags by main.cpp (the orchestrator
    // script knows the strategy file + git state, the backtester binary
    // doesn't). All optional; when unset, ResultsCollector falls back to
    // a window-only run_id and leaves the corresponding summary.json
    // fields empty.
    std::string strategy_name;
    std::string params_hash;
    std::string git_sha;
    std::string params_file;  // resolved params toml — copied into output dir
};

// Aeron streams used for backtest tick-gating.
// Backtester publishes BacktestControl on backtest_control and subscribes
// to BacktestAck on backtest_ack.  Stream IDs must match Strategy's config.
struct AeronConfig {
    // media_driver_dir moved to BaseSettings; kept streams-only.
    bpt::common::config::StreamConfig backtest_control{"aeron:ipc", 9002};  // pub: Backtester → Strategy
    bpt::common::config::StreamConfig backtest_ack{"aeron:ipc", 9001};      // sub: Strategy → Backtester
};

struct Settings {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    SimulationConfig simulation;
    DataConfig data;
    EndpointConfig endpoints;
    AeronConfig aeron;
    std::vector<InstrumentConfig> instruments;
    ResultsConfig results;
};

Settings load(const std::string& path);

}  // namespace bpt::backtester::config

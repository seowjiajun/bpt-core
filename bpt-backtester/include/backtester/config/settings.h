#pragma once

#include <cstdint>
#include <string>
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

// Simulated fill latency per venue.
struct SimLatencyConfig {
    uint32_t cex_base_ms{2};             // Binance, OKX, Deribit
    uint32_t hyperliquid_base_ms{200};   // On-chain latency floor
    uint32_t hyperliquid_jitter_ms{50};  // Random jitter added on top
};

struct SimulationConfig {
    std::string start;  // ISO8601 e.g. "2026-01-01T00:00:00Z"
    std::string end;    // ISO8601 e.g. "2026-01-31T23:59:59Z"
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
    // Per-fill fee in basis points of notional. Default 1.0 = HL maker fee
    // (0.01%, the most common case for AS / passive market making which
    // posts limit orders that rest in the book). For strategies that
    // aggressively cross the spread, set to taker fee (e.g. HL ~4.5 bps).
    // The model applies the same rate to every fill regardless of aggressor
    // side — a more principled split awaits a per-fill maker/taker flag
    // on FillReport.
    double fee_bps_per_fill{1.0};
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

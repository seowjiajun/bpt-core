#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <yggdrasil/aeron/stream_config.h>
#include <yggdrasil/logging.h>

namespace bpt::backtester::config {

// One entry per instrument Backtester will replay.
// exchange + symbol must exactly match the exchange-native symbol in the Parquet files.
struct InstrumentConfig {
    std::string exchange;  // BINANCE | OKX | HYPERLIQUID | DERIBIT
    std::string symbol;    // Exchange-native symbol e.g. BTCUSDT, BTC-USDT-SWAP, BTC
};

// Localhost WS ports that Huginn and Heimdall connect to instead of real exchanges.
struct EndpointConfig {
    uint16_t binance_md_port{9100};
    uint16_t okx_md_port{9101};
    uint16_t hyperliquid_md_port{9102};
    uint16_t deribit_md_port{9103};

    uint16_t binance_order_port{9110};
    uint16_t okx_order_port{9111};
    uint16_t hyperliquid_order_port{9112};
    uint16_t deribit_order_port{9113};
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
    uint32_t subscriber_wait_timeout_s{60};  // Wait up to N seconds for Huginn to connect
    SimLatencyConfig latency;
};

struct DataConfig {
    std::string local_cache{"/opt/bpt/data/backtest-cache"};
};

struct ResultsConfig {
    std::string output_dir{"results"};
    double starting_capital{100'000.0};
};

// Aeron streams used for backtest tick-gating.
// Backtester publishes BacktestControl on backtest_control and subscribes
// to BacktestAck on backtest_ack.  Stream IDs must match Strategy's config.
struct AeronConfig {
    std::string media_driver_dir;
    ygg::config::StreamConfig backtest_control{"aeron:ipc", 9002};  // pub: Backtester → Strategy
    ygg::config::StreamConfig backtest_ack{"aeron:ipc", 9001};      // sub: Strategy → Backtester
    int pub_timeout_ms{5000};
    int pub_poll_interval_ms{10};
};

struct Settings {
    SimulationConfig simulation;
    DataConfig data;
    EndpointConfig endpoints;
    AeronConfig aeron;
    std::vector<InstrumentConfig> instruments;
    ygg::logging::LogConfig logging;
    ResultsConfig results;
    uint16_t metrics_port{9105};
};

Settings load(const std::string& path);

}  // namespace bpt::backtester::config

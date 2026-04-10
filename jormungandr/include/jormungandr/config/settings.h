#pragma once

#include <yggdrasil/aeron/stream_config.h>

#include <cstdint>
#include <string>
#include <vector>

namespace jormungandr::config {

struct AeronConfig {
    std::string media_driver_dir;
    // Fenrir → Jormungandr: tick acknowledgement
    ygg::config::StreamConfig backtest_ack{"aeron:ipc", 9001};
    // Jormungandr → Fenrir: simulation control + clock
    ygg::config::StreamConfig backtest_control{"aeron:ipc", 9002};
};

// One entry per instrument Jormungandr will replay.
// exchange + symbol must exactly match the exchange-native symbol in the Parquet files.
struct InstrumentConfig {
    std::string exchange;  // BINANCE | OKX | HYPERLIQUID | DERIBIT
    std::string symbol;    // Exchange-native symbol e.g. BTCUSDT, BTC-USDT-SWAP, BTC
};

// Localhost WS ports that Huginn and Heimdall connect to instead of real exchanges.
// Each exchange gets two ports: one for MD (Huginn) and one for orders (Heimdall).
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
    std::string start;               // ISO8601 e.g. "2026-01-01T00:00:00Z"
    std::string end;                 // ISO8601 e.g. "2026-01-31T23:59:59Z"
    bool allow_partial_data{false};  // If true, clip missing symbol ranges rather than aborting
    SimLatencyConfig latency;
};

struct DataConfig {
    std::string local_cache{"/opt/bpt/data/backtest-cache"};
};

struct LoggingConfig {
    std::string level{"info"};  // trace | debug | info | warn | error | critical | off
    std::string dir{"logs"};
};

struct ResultsConfig {
    std::string output_dir{"results"};  // directory for trades.csv, pnl_curve.csv, summary.json
    double starting_capital{100'000.0};
};

struct Settings {
    AeronConfig aeron;
    SimulationConfig simulation;
    DataConfig data;
    EndpointConfig endpoints;
    std::vector<InstrumentConfig> instruments;
    LoggingConfig logging;
    ResultsConfig results;
    uint16_t metrics_port{9105};
};

Settings load(const std::string& path);

}  // namespace jormungandr::config

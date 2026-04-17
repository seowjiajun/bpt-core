#pragma once

#include <cstdint>
#include <string>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <vector>
#include <yggdrasil/aeron/stream_config.h>
#include <yggdrasil/logging.h>

// Forward declaration for ScheduleConfig::configured_exchanges_mask conversion.

namespace bpt::strategy {
namespace config {

struct AeronConfig {
    std::string media_driver_dir;
    // Reference data streams (required)
    ygg::config::StreamConfig refdata_control{"aeron:ipc", 1003};
    ygg::config::StreamConfig refdata_snapshot{"aeron:ipc", 1001};
    ygg::config::StreamConfig refdata_delta{"aeron:ipc", 1002};
    // Refdata live streams
    ygg::config::StreamConfig fee_schedule{"aeron:ipc", 1004};    // FeeSchedule id=19
    ygg::config::StreamConfig funding_rate{"aeron:ipc", 1005};    // FundingRate id=18
    ygg::config::StreamConfig refdata_status{"aeron:ipc", 1006};  // RefDataReady id=16, RefDataError id=17
    // Market data streams (optional — stream_id 0 means MD client is not started)
    ygg::config::StreamConfig md_control{"aeron:ipc", 0};
    ygg::config::StreamConfig md_data{"aeron:ipc", 0};
    ygg::config::StreamConfig md_ack_hb{"aeron:ipc", 0};
    // Order gateway streams (optional — stream_id 0 means Heimdall client is not started)
    ygg::config::StreamConfig order{"aeron:ipc", 0};             // Strategy → Heimdall
    ygg::config::StreamConfig exec_report{"aeron:ipc", 0};       // Heimdall → Strategy (ExecutionReport)
    ygg::config::StreamConfig heartbeat{"aeron:ipc", 0};         // Heimdall → Strategy (OrderGatewayHeartbeat)
    ygg::config::StreamConfig account_snapshot{"aeron:ipc", 0};  // Heimdall → Strategy (AccountSnapshot id=27)
    // Pricer vol surface streams (optional — stream_id 0 means vol surface client is not started)
    ygg::config::StreamConfig vol_surface{"aeron:ipc", 0};   // Pricer → Strategy (VolSurface id=21)
    ygg::config::StreamConfig pricer_status{"aeron:ipc", 0};  // PricerHeartbeat id=22, PricerReady id=23
    // Analytics toxicity stream (optional — stream_id 0 disables)
    ygg::config::StreamConfig toxicity{"aeron:ipc", 0};  // Analytics → Strategy (ToxicityUpdate)
    // Backtest streams (optional — only used when backtest_mode = true)
    ygg::config::StreamConfig backtest_control{"aeron:ipc", 9002};  // Backtester → Strategy (BacktestControl id=25)
    ygg::config::StreamConfig backtest_ack{"aeron:ipc", 9001};      // Strategy → Backtester (BacktestAck id=24)
    // Dashboard control (optional — stream_id 0 disables; bridge → Strategy)
    ygg::config::StreamConfig dashboard_control{"aeron:ipc", 9003};
    // Portfolio snapshot (optional — Strategy → bridge; published every ~100ms)
    ygg::config::StreamConfig dashboard_snapshot{"aeron:ipc", 9004};
    // Max time (ms) to wait for Aeron pub/sub registration before throwing.
    int pub_timeout_ms{5000};
    // Poll interval (ms) when waiting for Aeron pub/sub registration.
    int pub_poll_interval_ms{10};
};

// Per-venue execution parameters.
// Venue keys match exchange names used in refdata (e.g. "BINANCE", "OKX", "HYPERLIQUID").
struct VenueExecConfig {
    bool enabled{true};
    uint64_t latency_budget_us{1000};  // Expected fill latency; 200000+ for Hyperliquid, ~1000 for CEX
    std::string order_type{"LIMIT"};   // LIMIT or MARKET
    std::string tif{"GTC"};            // GTC, IOC, FOK
    uint32_t max_retries{3};
};

// Strategy-level risk limits.  All monetary values in USD-equivalent notional.
struct RiskConfig {
    double max_position_usd{10000.0};   // Max net position per instrument per venue
    double max_order_size_usd{1000.0};  // Max single order size
    double max_daily_loss_usd{500.0};   // Kill-switch: halt strategy if breached
    uint32_t max_open_orders{10};
};

// Startup and staleness gating.
struct ScheduleConfig {
    bool require_refdata_ready{true};                       // Block trading until Sindri snapshot received
    uint64_t md_staleness_threshold_ms{5000};               // Pause if no MD heartbeat within this window
    uint64_t max_refdata_staleness_ns{300'000'000'000ULL};  // 300 s: stale if fee/funding not updated

    // Bitmask of exchanges Strategy expects Sindri to have loaded.
    // bit0=BINANCE(0x01), bit1=OKX(0x02), bit2=HYPERLIQUID(0x04).
    // If RefDataReady.exchangesLoaded is missing any bit set here, Strategy halts.
    // Set from configured_exchanges list in config at load time.
    uint8_t configured_exchanges_mask{0x00};

    // Human-readable list from config ("BINANCE", "OKX", "HYPERLIQUID").
    // Converted to configured_exchanges_mask during AppConfig::load().
    std::vector<std::string> configured_exchanges;
};

struct StrategyConfig {
    std::string type;
    bool enabled{true};

    // Canonical instrument universe.  Format: BASE/QUOTE:TYPE  (e.g. BTC/USDT:SPOT).
    // Resolved against the Sindri snapshot at startup.
    // Empty = all instruments across md_exchanges (subscribe-all fallback).
    std::vector<std::string> instruments;

    // Exchanges to receive market data from.  Combined with instruments as an AND filter.
    // Empty = all exchanges.
    std::vector<std::string> md_exchanges;

    std::unordered_map<std::string, VenueExecConfig> venue_exec;  // keyed by exchange name
    RiskConfig risk;
    ScheduleConfig schedule;

    // Strategy-specific signal parameters (lookback windows, thresholds, etc.).
    // Parsed by the concrete strategy class.
    toml::table params;
};

struct EngineConfig {
    uint64_t correlation_id;
    StrategyConfig strategy;
};

struct AppConfig {
    AeronConfig aeron;
    EngineConfig strat;
    ygg::logging::LogConfig logging;
    int metrics_port{9104};
    bool backtest_mode{false};  // When true, Strategy gates on BacktestControl ticks from Backtester

    // Load configuration from YAML file
    // Throws std::runtime_error on parse failure
    [[nodiscard]] static AppConfig load(const std::string& path);
};

}  // namespace config
}  // namespace bpt::strategy

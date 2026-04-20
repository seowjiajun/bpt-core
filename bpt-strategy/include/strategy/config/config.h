#pragma once

#include <cstdint>
#include <string>
#include <toml++/toml.hpp>
#include <unordered_map>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>

// Forward declaration for ScheduleConfig::configured_exchanges_mask conversion.

namespace bpt::strategy {
namespace config {

struct AeronConfig {
    // media_driver_dir moved to BaseSettings; kept streams-only.
    // Reference data streams (required)
    bpt::common::config::StreamConfig refdata_control{"aeron:ipc", 1003};
    bpt::common::config::StreamConfig refdata_snapshot{"aeron:ipc", 1001};
    bpt::common::config::StreamConfig refdata_delta{"aeron:ipc", 1002};
    // Refdata live streams
    bpt::common::config::StreamConfig fee_schedule{"aeron:ipc", 1004};    // FeeSchedule id=19
    bpt::common::config::StreamConfig funding_rate{"aeron:ipc", 1005};    // FundingRate id=18
    bpt::common::config::StreamConfig refdata_status{"aeron:ipc", 1006};  // RefDataReady id=16, RefDataError id=17
    // Market data streams (optional — stream_id 0 means MD client is not started)
    bpt::common::config::StreamConfig md_control{"aeron:ipc", 0};
    bpt::common::config::StreamConfig md_data{"aeron:ipc", 0};
    bpt::common::config::StreamConfig md_ack_hb{"aeron:ipc", 0};
    // Order gateway streams (optional — stream_id 0 means OrderGateway client is not started)
    bpt::common::config::StreamConfig order{"aeron:ipc", 0};             // Strategy → OrderGateway
    bpt::common::config::StreamConfig exec_report{"aeron:ipc", 0};       // OrderGateway → Strategy (ExecutionReport)
    bpt::common::config::StreamConfig heartbeat{"aeron:ipc", 0};         // OrderGateway → Strategy (OrderGatewayHeartbeat)
    bpt::common::config::StreamConfig account_snapshot{"aeron:ipc", 0};  // OrderGateway → Strategy (AccountSnapshot id=27)
    // Pricer vol surface streams (optional — stream_id 0 means vol surface client is not started)
    bpt::common::config::StreamConfig vol_surface{"aeron:ipc", 0};   // Pricer → Strategy (VolSurface id=21)
    bpt::common::config::StreamConfig pricer_status{"aeron:ipc", 0};  // PricerHeartbeat id=22, PricerReady id=23
    // Analytics toxicity stream (optional — stream_id 0 disables)
    bpt::common::config::StreamConfig toxicity{"aeron:ipc", 0};  // Analytics → Strategy (ToxicityUpdate)
    // Backtest streams (optional — only used when backtest_mode = true)
    bpt::common::config::StreamConfig backtest_control{"aeron:ipc", 9002};  // Backtester → Strategy (BacktestControl id=25)
    bpt::common::config::StreamConfig backtest_ack{"aeron:ipc", 9001};      // Strategy → Backtester (BacktestAck id=24)
    // Dashboard control (optional — stream_id 0 disables; bridge → Strategy)
    bpt::common::config::StreamConfig dashboard_control{"aeron:ipc", 9003};
    // Portfolio snapshot (optional — Strategy → bridge; published every ~100ms)
    bpt::common::config::StreamConfig dashboard_snapshot{"aeron:ipc", 9004};
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
//
// These are declarative: strategies log them at startup so an operator can
// see what the STRATEGY expects its risk envelope to be. Actual enforcement
// lives in the order-gateway's risk module (`bpt-order-gateway/src/risk/`),
// driven by `[order-gateway.risk]` TOML fields. The split is intentional —
// risk enforcement must be single-point so a strategy bug can't bypass it.
//
// max_daily_loss_usd was REMOVED from this struct (2026-04-19): it existed
// as a declarative field that was parsed, logged, and never enforced,
// which was worse than not having it at all. The enforced daily-loss cap
// lives in `[order-gateway.risk].max_daily_loss_usd` (see PnlTracker).
struct RiskConfig {
    double max_position_usd{10000.0};   // Max net position per instrument per venue
    double max_order_size_usd{1000.0};  // Max single order size
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

// Warm-start / state persistence. Saves per-instrument EWMA + regime
// state to disk on graceful shutdown; reloads on startup subject to a
// TTL. Cuts the ~2 min vol/drift/kappa warmup gap on restarts.
struct WarmStartConfig {
    // Directory that holds <correlation_id>.json state files. Empty
    // string disables the feature — save/load become no-ops, and a
    // cold start rebuilds all EWMA state from scratch.
    std::string state_dir;

    // Discard saved state if (now - saved_at) exceeds this. EWMA
    // vol_halflife defaults to 60 s, so anything past ~5 half-lives
    // (~5 min) has decayed to ~3% of its original weight and is less
    // useful than a fresh warmup. 600 s = 10 min is a conservative
    // default; tune if your vol_halflife differs materially.
    uint64_t max_age_s{600};
};

struct StrategyConfig {
    std::string type;
    bool enabled{true};

    // Canary / shadow mode: when true, orders are routed to an
    // in-process PaperOrderGatewayClient instead of bpt-order-gateway.
    // Fills are synthesised from the MD stream by PaperFillEngine, so
    // the strategy's lifecycle runs unchanged without touching the
    // exchange. Use for regression-testing a new strategy variant
    // alongside live before flipping it in.
    bool paper_mode{false};

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
    WarmStartConfig warm_start;

    // Strategy-specific signal parameters (lookback windows, thresholds, etc.).
    // Parsed by the concrete strategy class.
    toml::table params;
};

struct EngineConfig {
    uint64_t correlation_id;
    StrategyConfig strategy;
};

struct AppConfig {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    AeronConfig aeron;
    EngineConfig strat;
    bool backtest_mode{false};  // When true, Strategy gates on BacktestControl ticks from Backtester

    // Load configuration from YAML file
    // Throws std::runtime_error on parse failure
    [[nodiscard]] static AppConfig load(const std::string& path);
};

}  // namespace config
}  // namespace bpt::strategy

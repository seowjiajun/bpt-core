#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <bpt_common/aeron/stream_config.h>
#include <bpt_common/logging.h>

namespace bpt::order_gateway::config {

struct AeronConfig {
    std::string media_driver_dir;
    bpt::common::config::StreamConfig order{"aeron:ipc", 3001};
    bpt::common::config::StreamConfig exec_report{"aeron:ipc", 3002};
    bpt::common::config::StreamConfig heartbeat{"aeron:ipc", 3003};
    bpt::common::config::StreamConfig account_snapshot{"aeron:ipc", 3004};
};

struct RiskConfig {
    bool trading_enabled{true};
    double max_order_size_usd{1000.0};
    double max_notional_per_order_usd{5000.0};
    uint32_t max_open_orders_per_venue{50};
    uint32_t max_orders_per_second{10};
    // Daily-loss kill switch. Computed from realized P&L on fills via
    // PnlTracker. When breached, RiskChecker.set_trading_enabled(false)
    // is flipped and all subsequent orders reject with RISK_REJECTED.
    // The latch is intentionally not auto-cleared at UTC midnight — a
    // human must restart the service so someone looks at what happened.
    // 0 disables the check.
    double max_daily_loss_usd{0.0};
    // Pretrade check: rejects a NewOrder if the PROJECTED position
    // (current net_qty + order direction × order_qty) × order_price
    // would exceed this cap in absolute terms. 0 disables.
    // Uses the order's own price as the mark, which is exact at fill
    // time. Distinct from max_order_size_usd (caps a single order's
    // notional) and max_notional_per_order_usd (redundant alias at
    // this point, worth collapsing).
    double max_position_usd{0.0};

    // Exchange-reject-rate circuit breaker. Trips when the share of
    // post-send REJECTED exec reports over the rolling window exceeds
    // the threshold (and at least min_events have landed). On trip:
    // RiskChecker.set_trading_enabled(false) — same latch the daily-loss
    // kill switch uses. Intentionally disabled by default until a real
    // venue has been observed long enough to pick sane thresholds;
    // enable per-env in the TOML once calibrated. min_events guards
    // against tripping on 1-of-1 false positives during warmup.
    bool     reject_rate_breaker_enabled{false};
    double   reject_rate_threshold_pct{20.0};
    uint32_t reject_rate_window_sec{60};
    uint32_t reject_rate_min_events{10};

    // Per-adapter disconnect-rate circuit breaker. Trips when an
    // exchange adapter has reconnected at least `threshold` times in
    // the last `window_sec` — signals a persistent loop (expired
    // creds, geo-block, margin-mode mismatch) where reconnecting
    // only burns rate-limit. On trip, only orders to that adapter
    // reject (unlike the reject-rate breaker which halts all trading);
    // other venues keep working. Restart required to clear.
    // Same config is shared across all adapters — threshold tuning
    // is not venue-specific yet.
    bool     disconnect_breaker_enabled{false};
    uint32_t disconnect_threshold{5};
    uint32_t disconnect_window_sec{60};
};

struct AdapterConfig {
    std::string exchange;
    std::string secret_name;  // systemd-creds name (slashes normalized to '-'); e.g. "bpt/testnet/OKX" → bpt-testnet-OKX
    bool testnet{false};
    std::string rest_host;
    std::string rest_port{"443"};
    std::string ws_host;
    std::string ws_port{"443"};
    std::string ws_path;
    bool use_tls{true};
    int io_cpu{-1};  // CPU to pin the adapter IO thread to; -1 = no pinning

    // SPSC ring buffer between the adapter IO thread and the main poll
    // thread. Must be a power of 2. Default 256 covers any realistic
    // burst — a full book snapshot on a cold reconnect is ~tens of
    // events — but a high-churn strategy on a busy venue may want 1024+.
    uint32_t exec_queue_capacity{256};
};

struct GatewayConfig {
    uint32_t heartbeat_interval_ms{1000};
    uint32_t stale_order_timeout_ms{30000};
    int poll_cpu{-1};  // CPU to pin the main poll loop thread to; -1 = no pinning
    RiskConfig risk;
    std::vector<AdapterConfig> adapters;
};

struct Settings {
    std::string environment;             // "prod" | "qa" | "dev" — logged at startup, validated against
                                         // exchange_config
    std::vector<std::string> exchanges;  // exchanges to activate from exchange_config (e.g. ["OKX", "BINANCE"])
    AeronConfig aeron;
    GatewayConfig gateway;
    bpt::common::logging::LogConfig logging;
    uint16_t metrics_port{9103};
};

Settings load(const std::string& path);

}  // namespace bpt::order_gateway::config

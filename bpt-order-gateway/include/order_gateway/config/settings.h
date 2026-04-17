#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <yggdrasil/aeron/stream_config.h>
#include <yggdrasil/logging.h>

namespace bpt::order_gateway::config {

struct AeronConfig {
    std::string media_driver_dir;
    ygg::config::StreamConfig order{"aeron:ipc", 3001};
    ygg::config::StreamConfig exec_report{"aeron:ipc", 3002};
    ygg::config::StreamConfig heartbeat{"aeron:ipc", 3003};
    ygg::config::StreamConfig account_snapshot{"aeron:ipc", 3004};
};

struct RiskConfig {
    bool trading_enabled{true};
    double max_order_size_usd{1000.0};
    double max_notional_per_order_usd{5000.0};
    uint32_t max_open_orders_per_venue{50};
    uint32_t max_orders_per_second{10};
};

struct AdapterConfig {
    std::string exchange;
    std::string secret_name;  // AWS Secrets Manager secret name, e.g. "bpt/testnet/OKX"
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
    ygg::logging::LogConfig logging;
    uint16_t metrics_port{9103};
};

Settings load(const std::string& path);

}  // namespace bpt::order_gateway::config

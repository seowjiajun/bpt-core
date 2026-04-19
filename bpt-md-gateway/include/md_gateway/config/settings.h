#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <yggdrasil/aeron/stream_config.h>
#include <yggdrasil/logging.h>

namespace bpt::md_gateway::config {

struct AeronConfig {
    std::string media_driver_dir;
    ygg::config::StreamConfig control{"aeron:ipc", 2001};       // Strategy → MdGateway subscription control
    ygg::config::StreamConfig data{"aeron:ipc", 2002};          // MdGateway → Strategy market data
    ygg::config::StreamConfig ack_hb{"aeron:ipc", 2003};        // MdGateway → Strategy acks + heartbeats
    ygg::config::StreamConfig funding_rate{"aeron:ipc", 1005};  // MdGateway → Strategy funding rates
};

struct AdapterConfig {
    std::string exchange;  // e.g. BINANCE, OKX, HYPERLIQUID
    std::string ws_host;
    std::string ws_port;
    std::string ws_path;
    bool use_tls{true};
    // Timeout (ms) for the entire WS connect sequence: DNS + TCP + TLS + WS upgrade.
    // If any step hangs longer than this, ws_connect throws and the adapter reconnects.
    uint32_t ws_connect_timeout_ms{30000};

    // How often the adapter sends a text "ping" to OKX (ms).
    // OKX closes with code 4004 if no data is received from the client for 30s.
    // Must be well under 30 000.
    uint32_t ws_ping_interval_ms{25000};
    // Read timeout per receive-loop iteration (ms).
    // Kept short so the loop can send proactive pings on schedule and respond
    // promptly to stop requests, but long enough not to race with OKX's close frame.
    uint32_t ws_read_timeout_ms{5000};
    // Maximum silence before the adapter treats the connection as dead and
    // reconnects.  Should be comfortably above ws_read_timeout_ms and any
    // expected quiet-market gaps.  60s is conservative; tighten per exchange.
    uint32_t ws_liveness_timeout_ms{60000};

    // Maximum allowed price deviation (%) between consecutive ticks for the
    // same instrument.  Ticks that move the mid price by more than this
    // percentage are dropped and logged.  Set to 0 to disable the check.
    double max_price_deviation_pct{10.0};

    // CPU core to pin the adapter's IO thread to (-1 = no pinning).
    // Set to an isolated core (isolcpus kernel param) to eliminate scheduler
    // jitter on the hot receive path.
    int io_thread_cpu{-1};

    // Receive socket buffer size in bytes (0 = OS default).
    // Increase to absorb exchange burst traffic without kernel drops.
    // Check /proc/sys/net/core/rmem_max for the system ceiling.
    uint32_t so_rcvbuf_bytes{0};

    // Validation-drop circuit breaker. Trips when the share of publishes
    // rejected by MdValidator over the rolling window exceeds the
    // threshold (and at least min_events have landed). On trip: the
    // ValidatingPublisher stops forwarding to Aeron for this adapter —
    // downstream sees no data rather than bad data. Latches; restart
    // to clear. Disabled by default until drop rates are characterized
    // per-venue on a live feed (schemas change, symbol listings shift).
    bool     validation_drop_breaker_enabled{false};
    double   validation_drop_threshold_pct{30.0};
    uint32_t validation_drop_window_sec{60};
    uint32_t validation_drop_min_events{50};
};

struct Settings {
    std::string environment;             // "prod" | "qa" | "dev" — logged at startup, validated against exchange_cfg
    std::vector<std::string> exchanges;  // exchanges to activate from exchange_config (e.g. ["OKX", "BINANCE"])
    AeronConfig aeron;
    std::vector<AdapterConfig> adapters;

    // Interval (ms) between per-instrument subscription heartbeats on the ack/hb stream.
    uint32_t subscription_heartbeat_interval_ms{5000};

    // Interval (ms) between MdServiceHeartbeat messages on the ack/hb stream.
    uint32_t service_heartbeat_interval_ms{5000};

    ygg::logging::LogConfig logging;

    // Prometheus metrics endpoint.
    // Restrict host to "127.0.0.1" in prod to prevent exposure to external networks.
    // Use "0.0.0.0" only if a separate scrape network interface is needed.
    std::string metrics_host{"127.0.0.1"};
    uint16_t metrics_port{9102};
};

Settings load(const std::string& path);

}  // namespace bpt::md_gateway::config

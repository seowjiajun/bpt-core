#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>

namespace bpt::md_gateway::config {

struct AeronConfig {
    // media_driver_dir moved to BaseSettings; kept streams-only.
    bpt::common::config::StreamConfig control{"aeron:ipc", 2001};       // Strategy → MdGateway subscription control
    bpt::common::config::StreamConfig data{"aeron:ipc", 2002};          // MdGateway → Strategy market data
    bpt::common::config::StreamConfig ack_hb{"aeron:ipc", 2003};        // MdGateway → Strategy acks + heartbeats
    bpt::common::config::StreamConfig funding_rate{"aeron:ipc", 1005};  // MdGateway → Strategy funding rates
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

    // Optional TLS-pinning allowlist. When non-empty, the TLS handshake
    // rejects any leaf cert whose SHA-256 fingerprint is not in this
    // list, on top of the standard CA + hostname verification. Values
    // are lowercase hex, 64 chars each, no colons — matches
    //   openssl x509 -fingerprint -sha256 -noout | tr -d : | tr A-F a-f
    // Empty (default) = no pinning. Operator must rotate pins when the
    // exchange rotates its TLS cert (typically every 1-3 years).
    std::vector<std::string> pinned_tls_sha256;

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

// Raw-WS-frame recording. When enabled, every adapter tees each received
// frame to disk via bpt::md_gateway::recorder::RawSpool. Trading instances
// run with enabled=false (zero overhead). Recording-rig instances run with
// enabled=true alongside a broader static_subscriptions list.
struct RecordingConfig {
    bool enabled{false};
    std::string output_dir{"/opt/bpt/data/raw"};
    uint32_t rotate_interval_seconds{3600};
    uint32_t fsync_interval_ms{1000};
    uint32_t buffer_bytes{1u << 20};
    uint32_t checkpoint_interval_seconds{30};
};

// Subscription declared entirely in md-gateway config. Applied on startup
// in standalone mode, where no external caller sends MdSubscribeBatch.
// instrument_id is operator-assigned — pick any uint64 stable for your
// recording corpus; it's stamped onto every frame for that symbol.
struct StaticSubscription {
    uint64_t instrument_id{0};
    std::string exchange;
    std::string symbol;
    uint8_t depth{0};
};

struct Settings {
    // Shared lifecycle config (environment, media_driver_dir, logging,
    // metrics_port, calibrate_tsc). Populated by bpt::app::load_base_settings().
    bpt::app::BaseSettings base;

    std::vector<std::string> exchanges;  // exchanges to activate from exchange_config (e.g. ["OKX", "BINANCE"])
    AeronConfig aeron;
    std::vector<AdapterConfig> adapters;

    // Non-empty = standalone mode. Subscriptions are applied once at startup.
    // The control-stream subscriber remains wired so an external caller could
    // still override at runtime; recording rigs typically run without one.
    std::vector<StaticSubscription> static_subscriptions;

    // Raw-frame recording. Disabled by default — only recording-rig instances
    // flip enabled=true. See RecordingConfig.
    RecordingConfig recording;

    // Interval (ms) between per-instrument subscription heartbeats on the ack/hb stream.
    uint32_t subscription_heartbeat_interval_ms{5000};

    // Interval (ms) between MdServiceHeartbeat messages on the ack/hb stream.
    uint32_t service_heartbeat_interval_ms{5000};

    // Prometheus metrics bind host. Port comes from base.metrics_port.
    // Restrict host to "127.0.0.1" in prod to prevent exposure to external networks.
    // Use "0.0.0.0" only if a separate scrape network interface is needed.
    std::string metrics_host{"127.0.0.1"};
};

Settings load(const std::string& path);

}  // namespace bpt::md_gateway::config

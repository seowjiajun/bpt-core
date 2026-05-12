#pragma once

/// \file
/// \brief Loaded configuration for `bpt-md-gateway` (TOML → struct).
///
/// One Settings is built per process by `config::load(path)`. The
/// structure mirrors the TOML layout one-to-one — each section here
/// has a matching `[section]` block in the gateway's TOML file —
/// so operators reading a config can map every key back to its
/// in-memory home without grepping. Validation that's beyond
/// "syntactically present" runs inside `load()` rather than each
/// consumer.

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <string>
#include <vector>

namespace bpt::md_gateway::config {

/// \brief Aeron stream IDs the gateway publishes / subscribes on.
///
/// `media_driver_dir` is owned by `BaseSettings` (process-shared);
/// only the per-stream channel + ID lives here.
struct AeronConfig {
    bpt::common::config::StreamConfig control{"aeron:ipc", 2001};       ///< Strategy → MdGateway: subscription control
    bpt::common::config::StreamConfig data{"aeron:ipc", 2002};          ///< MdGateway → Strategy: market data
    bpt::common::config::StreamConfig ack_hb{"aeron:ipc", 2003};        ///< MdGateway → Strategy: acks + heartbeats
    bpt::common::config::StreamConfig funding_rate{"aeron:ipc", 1005};  ///< MdGateway → Strategy: funding rates
};

/// \brief Per-exchange adapter configuration.
///
/// One AdapterConfig per active venue. The gateway runs one adapter
/// per entry; the exchange string here is the key Settings::exchanges
/// matches against to decide whether to instantiate this adapter.
struct AdapterConfig {
    std::string exchange;  ///< venue label (e.g. "BINANCE", "OKX", "HYPERLIQUID")
    std::string ws_host;
    std::string ws_port;
    std::string ws_path;
    bool use_tls{true};

    /// \brief Timeout (ms) for the entire WS connect sequence: DNS + TCP + TLS + WS upgrade.
    ///
    /// If any step hangs longer than this, ws_connect throws and the
    /// adapter reconnects.
    uint32_t ws_connect_timeout_ms{30000};

    /// \brief How often the adapter sends a text "ping" to the venue (ms).
    ///
    /// Must be well under any venue-side idle-disconnect threshold —
    /// e.g. OKX closes with code 4004 after 30 s of client silence,
    /// so the default of 25 s leaves a 5 s margin.
    uint32_t ws_ping_interval_ms{25000};

    /// \brief Read timeout per receive-loop iteration (ms).
    ///
    /// Kept short so the loop sends proactive pings on schedule and
    /// responds promptly to stop requests, but long enough not to
    /// race with the venue's close frame.
    uint32_t ws_read_timeout_ms{5000};

    /// \brief Maximum silence before the adapter treats the connection as dead and reconnects.
    ///
    /// Should be comfortably above `ws_read_timeout_ms` and any
    /// expected quiet-market gaps. 60 s is conservative; tighten
    /// per exchange.
    uint32_t ws_liveness_timeout_ms{60000};

    /// \brief Maximum allowed price deviation (%) between consecutive ticks for an instrument.
    ///
    /// Ticks that move the mid price by more than this percentage are
    /// dropped and logged. Set to 0 to disable the check.
    double max_price_deviation_pct{10.0};

    /// \brief CPU core to pin the adapter's IO thread to (-1 = no pinning).
    ///
    /// Set to an isolated core (`isolcpus` kernel param) to eliminate
    /// scheduler jitter on the hot receive path.
    int io_thread_cpu{-1};

    /// \brief Receive socket buffer size in bytes (0 = OS default).
    ///
    /// Increase to absorb exchange burst traffic without kernel drops.
    /// Check `/proc/sys/net/core/rmem_max` for the system ceiling.
    uint32_t so_rcvbuf_bytes{0};

    /// \brief Optional TLS-pinning allowlist (lowercase hex SHA-256).
    ///
    /// When non-empty, the TLS handshake rejects any leaf cert whose
    /// SHA-256 fingerprint is not in this list, on top of the standard
    /// CA + hostname verification. Format: 64-char lowercase hex,
    /// no colons — matches
    /// `openssl x509 -fingerprint -sha256 -noout | tr -d : | tr A-F a-f`.
    /// Empty (default) = no pinning. Operator must rotate pins when
    /// the exchange rotates its TLS cert (typically every 1–3 years).
    std::vector<std::string> pinned_tls_sha256;

    /// \name Validation-drop circuit breaker
    /// \brief Per-adapter latch on sustained MdValidator drop rates.
    ///
    /// Trips when the share of publishes rejected by MdValidator over
    /// the rolling window exceeds the threshold (and at least
    /// `min_events` have landed). On trip the ValidatingPublisher
    /// stops forwarding to Aeron — downstream sees no data rather
    /// than bad data. Latches; restart to clear. Disabled by default
    /// until drop rates are characterised per-venue on a live feed
    /// (schemas change, symbol listings shift).
    /// @{
    bool validation_drop_breaker_enabled{false};
    double validation_drop_threshold_pct{30.0};
    uint32_t validation_drop_window_sec{60};
    uint32_t validation_drop_min_events{50};
    /// @}
};

/// \brief Top-level loaded settings.
struct Settings {
    /// \brief Shared lifecycle config (env, media_driver_dir, logging, metrics_port, calibrate_tsc).
    ///
    /// Populated by `bpt::app::load_base_settings()` before the
    /// gateway-specific fields are filled in.
    bpt::app::BaseSettings base;

    /// \brief Exchanges to activate (subset of names that appear as `AdapterConfig::exchange` keys).
    ///
    /// Adapters whose `exchange` is not in this list are skipped at
    /// startup. Lets the same TOML run with different venue subsets
    /// across hosts.
    std::vector<std::string> exchanges;

    AeronConfig aeron;
    std::vector<AdapterConfig> adapters;

    /// \brief Interval (ms) between per-instrument subscription heartbeats on the ack/hb stream.
    uint32_t subscription_heartbeat_interval_ms{5000};

    /// \brief Interval (ms) between MdServiceHeartbeat messages on the ack/hb stream.
    uint32_t service_heartbeat_interval_ms{5000};

    /// \brief Prometheus metrics bind host. Port comes from `base.metrics_port`.
    ///
    /// Restrict to "127.0.0.1" in prod to prevent exposure to external
    /// networks. Use "0.0.0.0" only if a separate scrape network
    /// interface is needed.
    std::string metrics_host{"127.0.0.1"};
};

/// \brief Load and validate a Settings instance from a TOML file at `path`.
///
/// Throws on any structural problem — missing required keys, contradictory
/// adapter entries, prod env paired with a testnet exchange catalog, etc.
Settings load(const std::string& path);

}  // namespace bpt::md_gateway::config

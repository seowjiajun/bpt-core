#include "md_gateway/config/settings.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/streams_map.h>
#include <bpt_common/config/profile_config.h>
#include <bpt_common/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <unordered_set>

namespace bpt::md_gateway::config {

Settings load(const std::string& path) {
    toml::table root = toml::parse_file(path);

    bpt::common::config::ProfileConfig profile;
    bool have_profile = false;
    if (auto v = root["profile_config"].value<std::string>()) {
        profile = bpt::common::config::load_profile_config(*v);
        have_profile = true;
        bpt::common::log::info("Loaded deployment profile from {} (env={}, exchanges=[{}])",
                               *v,
                               bpt::common::to_string(profile.environment),
                               fmt::join(profile.exchanges, ", "));
        if (!root.contains("environment"))
            root.insert("environment", std::string(bpt::common::to_string(profile.environment)));
    }

    // Load raw adapter list from exchange config file (per-TOML override
    // wins over profile.exchange_config), falling back to inline.
    std::string exchange_config_path = profile.exchange_config;
    if (auto v = root["exchange_config"].value<std::string>())
        exchange_config_path = *v;
    toml::array adapters_arr;
    if (!exchange_config_path.empty()) {
        toml::table exchange = toml::parse_file(exchange_config_path);
        if (auto* arr = exchange["adapters"].as_array())
            adapters_arr = *arr;
    } else if (auto* arr = root["adapters"].as_array()) {
        adapters_arr = *arr;
    }

    Settings s;
    bpt::app::load_base_settings(root, s.base);

    if (!exchange_config_path.empty()) {
        const bool path_has_live = exchange_config_path.find("live") != std::string::npos;
        const bool path_has_testnet = exchange_config_path.find("testnet") != std::string::npos;
        // prod environment paired with a testnet exchange_config is the
        // catastrophic misdeploy — fail boot so the mistake is impossible
        // to miss. The inverse (qa/dev with live) is usually intentional
        // during staging; keep that as a warning.
        if (s.base.is_prod() && path_has_testnet)
            throw std::runtime_error(
                fmt::format("environment = \"prod\" but exchange_config = \"{}\" resolves to a testnet path — "
                            "refusing to start (prevents prod → testnet misdeploy)",
                            exchange_config_path));
        if (!s.base.is_prod() && path_has_live)
            bpt::common::log::warn("environment = \"{}\" but exchange_config = \"{}\" — possible misconfiguration",
                                   bpt::app::to_string(s.base.environment),
                                   exchange_config_path);
    }

    bpt::common::log::info("Environment: {}", bpt::app::to_string(s.base.environment));

    // Read the exchanges filter — per-TOML override wins over profile.
    std::unordered_set<std::string> exchange_filter;
    if (auto* arr = root["exchanges"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                exchange_filter.insert(*v);
        s.exchanges = {exchange_filter.begin(), exchange_filter.end()};
    } else if (have_profile) {
        for (const auto& ex : profile.exchanges)
            exchange_filter.insert(ex);
        s.exchanges = profile.exchanges;
    }
    if (!s.exchanges.empty())
        bpt::common::log::info("Exchange filter: [{}]", fmt::join(s.exchanges, ", "));

    bpt::common::config::AeronStreamMap shared_streams;
    if (auto v = root["aeron_config"].value<std::string>()) {
        shared_streams = bpt::common::config::load_shared_streams(*v);
        bpt::common::log::info("Loaded shared aeron stream map from {} ({} streams)",
                               *v, shared_streams.stream_ids.size());
        if (!shared_streams.media_driver_dir.empty())
            s.base.media_driver_dir = shared_streams.media_driver_dir;
    }

    using bpt::common::config::resolve_stream;
    s.aeron.md_control   = resolve_stream(shared_streams, "md_control",   2001);
    s.aeron.md_data      = resolve_stream(shared_streams, "md_data",      2002);
    s.aeron.md_ack_hb    = resolve_stream(shared_streams, "md_ack_hb",    2003);
    s.aeron.funding_rate = resolve_stream(shared_streams, "funding_rate", 1005);

    for (auto& elem : adapters_arr) {
        auto* a = elem.as_table();
        if (!a)
            continue;

        auto exchange_name = (*a)["exchange"].value<std::string>().value_or("");

        if (!exchange_filter.count(exchange_name))
            continue;

        AdapterConfig ac;
        ac.exchange = exchange_name;
        if (auto v = (*a)["ws_host"].value<std::string>())
            ac.ws_host = *v;
        if (auto v = (*a)["ws_port"].value<std::string>())
            ac.ws_port = *v;
        if (auto v = (*a)["ws_path"].value<std::string>())
            ac.ws_path = *v;
        if (auto v = (*a)["use_tls"].value<bool>())
            ac.use_tls = *v;
        if (auto v = (*a)["ws_connect_timeout_ms"].value<int64_t>())
            ac.ws_connect_timeout_ms = static_cast<uint32_t>(*v);
        if (auto v = (*a)["ws_read_timeout_ms"].value<int64_t>())
            ac.ws_read_timeout_ms = static_cast<uint32_t>(*v);
        if (auto v = (*a)["ws_ping_interval_ms"].value<int64_t>())
            ac.ws_ping_interval_ms = static_cast<uint32_t>(*v);
        if (auto v = (*a)["ws_liveness_timeout_ms"].value<int64_t>())
            ac.ws_liveness_timeout_ms = static_cast<uint32_t>(*v);
        if (auto v = (*a)["io_thread_cpu"].value<int64_t>())
            ac.io_thread_cpu = static_cast<int>(*v);
        if (auto v = (*a)["so_rcvbuf_bytes"].value<int64_t>())
            ac.so_rcvbuf_bytes = static_cast<uint32_t>(*v);
        if (auto v = (*a)["max_price_deviation_pct"].value<double>())
            ac.max_price_deviation_pct = *v;
        if (auto v = (*a)["validation_drop_breaker_enabled"].value<bool>())
            ac.validation_drop_breaker_enabled = *v;
        if (auto v = (*a)["validation_drop_threshold_pct"].value<double>())
            ac.validation_drop_threshold_pct = *v;
        if (auto v = (*a)["validation_drop_window_sec"].value<int64_t>())
            ac.validation_drop_window_sec = static_cast<uint32_t>(*v);
        if (auto v = (*a)["validation_drop_min_events"].value<int64_t>())
            ac.validation_drop_min_events = static_cast<uint32_t>(*v);
        if (auto* arr = (*a)["pinned_tls_sha256"].as_array()) {
            for (auto& elem : *arr)
                if (auto v = elem.value<std::string>())
                    ac.pinned_tls_sha256.push_back(*v);
        }

        // Validate required connectivity fields — throw at boot so the
        // operator sees the bad TOML immediately rather than discovering
        // it via a silently-missing MD feed at runtime. Previously we
        // skipped the adapter and logged an error; that left the service
        // running in a half-configured state.
        if (ac.ws_host.empty() || ac.ws_port.empty() || ac.ws_path.empty())
            throw std::runtime_error(fmt::format("Adapter {} missing required ws_host/ws_port/ws_path", exchange_name));
        if (!ac.use_tls)
            bpt::common::log::warn("Adapter {} has use_tls=false — TLS is enforced regardless; update config",
                                   exchange_name);

        s.adapters.push_back(std::move(ac));
    }

    if (auto v = root["subscription_heartbeat_interval_ms"].value<int64_t>())
        s.subscription_heartbeat_interval_ms = static_cast<uint32_t>(*v);
    if (auto v = root["service_heartbeat_interval_ms"].value<int64_t>())
        s.service_heartbeat_interval_ms = static_cast<uint32_t>(*v);

    // metrics.host is md-gateway-specific (bind address for the exposer);
    // metrics.port is read into base.metrics_port by load_base_settings.
    if (auto* m = root["metrics"].as_table()) {
        if (auto v = (*m)["host"].value<std::string>())
            s.metrics_host = *v;
    }

    return s;
}

}  // namespace bpt::md_gateway::config

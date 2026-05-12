#include "order_gateway/config/settings.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <unordered_set>

namespace bpt::order_gateway::config {

namespace {

bpt::common::config::StreamConfig load_stream(const toml::table* t,
                                              std::string default_channel,
                                              int32_t default_stream_id) {
    bpt::common::config::StreamConfig s{std::move(default_channel), default_stream_id};
    if (!t)
        return s;
    if (auto v = (*t)["channel"].value<std::string>())
        s.channel = *v;
    if (auto v = (*t)["stream_id"].value<int64_t>())
        s.stream_id = static_cast<int32_t>(*v);
    return s;
}

}  // namespace

Settings load(const std::string& path) {
    toml::table root = toml::parse_file(path);

    // Load raw adapter list from exchange config file, falling back to inline.
    std::string exchange_config_path;
    toml::array adapters_arr;
    if (auto v = root["exchange_config"].value<std::string>()) {
        exchange_config_path = *v;
        toml::table exchange = toml::parse_file(*v);
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
        // Fatal: prod environment with a testnet exchange_config. Any
        // live trades would hit the testnet venue (or vice versa — the
        // order-gateway is the worst place to be uncertain about where
        // orders go).
        if (s.base.is_prod() && path_has_testnet)
            throw std::runtime_error(
                fmt::format("environment = \"prod\" but exchange_config = \"{}\" resolves to a testnet path — "
                            "refusing to start",
                            exchange_config_path));
        if (!s.base.is_prod() && path_has_live)
            bpt::common::log::warn("environment = \"{}\" but exchange_config = \"{}\" — possible misconfiguration",
                                   bpt::app::to_string(s.base.environment),
                                   exchange_config_path);
    }

    bpt::common::log::info("Environment: {}", bpt::app::to_string(s.base.environment));

    // Read the exchanges filter from the instance config.
    std::unordered_set<std::string> exchange_filter;
    if (auto* arr = root["exchanges"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                exchange_filter.insert(*v);
        s.exchanges = {exchange_filter.begin(), exchange_filter.end()};
        bpt::common::log::info("Exchange filter: [{}]", fmt::join(s.exchanges, ", "));
    }

    if (auto* aeron = root["aeron"].as_table()) {
        s.aeron.order = load_stream((*aeron)["order"].as_table(), "aeron:ipc", 3001);
        s.aeron.exec_report = load_stream((*aeron)["exec_report"].as_table(), "aeron:ipc", 3002);
        s.aeron.heartbeat = load_stream((*aeron)["heartbeat"].as_table(), "aeron:ipc", 3003);
        s.aeron.account_snapshot = load_stream((*aeron)["account_snapshot"].as_table(), "aeron:ipc", 3004);
    }

    // TOML section is [order-gateway] / [order-gateway.risk] across every
    // config file in bpt-order-gateway/config/. Previously the loader
    // looked up root["gateway"] here, which never matches — so the entire
    // gateway + risk block was silently ignored and the service ran on the
    // hardcoded defaults in Settings (trading_enabled=true,
    // max_daily_loss_usd=0.0 = DISABLED). That left the daily-loss kill
    // switch effectively never armed in prod. Now reads the correct key.
    if (auto* g = root["order-gateway"].as_table()) {
        if (auto v = (*g)["heartbeat_interval_ms"].value<int64_t>())
            s.gateway.heartbeat_interval_ms = static_cast<uint32_t>(*v);
        if (auto v = (*g)["stale_order_timeout_ms"].value<int64_t>())
            s.gateway.stale_order_timeout_ms = static_cast<uint32_t>(*v);

        if (auto* r = (*g)["risk"].as_table()) {
            if (auto v = (*r)["trading_enabled"].value<bool>())
                s.gateway.risk.trading_enabled = *v;
            if (auto v = (*r)["max_order_size_usd"].value<double>())
                s.gateway.risk.max_order_size_usd = *v;
            if (auto v = (*r)["max_notional_per_order_usd"].value<double>())
                s.gateway.risk.max_notional_per_order_usd = *v;
            if (auto v = (*r)["max_open_orders_per_venue"].value<int64_t>())
                s.gateway.risk.max_open_orders_per_venue = static_cast<uint32_t>(*v);
            if (auto v = (*r)["max_orders_per_second"].value<int64_t>())
                s.gateway.risk.max_orders_per_second = static_cast<uint32_t>(*v);
            if (auto v = (*r)["max_daily_loss_usd"].value<double>())
                s.gateway.risk.max_daily_loss_usd = *v;
            if (auto v = (*r)["max_position_usd"].value<double>())
                s.gateway.risk.max_position_usd = *v;
            if (auto v = (*r)["reject_rate_breaker_enabled"].value<bool>())
                s.gateway.risk.reject_rate_breaker_enabled = *v;
            if (auto v = (*r)["reject_rate_threshold_pct"].value<double>())
                s.gateway.risk.reject_rate_threshold_pct = *v;
            if (auto v = (*r)["reject_rate_window_sec"].value<int64_t>())
                s.gateway.risk.reject_rate_window_sec = static_cast<uint32_t>(*v);
            if (auto v = (*r)["reject_rate_min_events"].value<int64_t>())
                s.gateway.risk.reject_rate_min_events = static_cast<uint32_t>(*v);
            if (auto v = (*r)["disconnect_breaker_enabled"].value<bool>())
                s.gateway.risk.disconnect_breaker_enabled = *v;
            if (auto v = (*r)["disconnect_threshold"].value<int64_t>())
                s.gateway.risk.disconnect_threshold = static_cast<uint32_t>(*v);
            if (auto v = (*r)["disconnect_window_sec"].value<int64_t>())
                s.gateway.risk.disconnect_window_sec = static_cast<uint32_t>(*v);
        }
    }

    for (auto& elem : adapters_arr) {
        auto* a = elem.as_table();
        if (!a)
            continue;

        auto exchange_name = (*a)["exchange"].value<std::string>().value_or("");

        if (!exchange_filter.count(exchange_name))
            continue;

        AdapterConfig ac;
        ac.exchange = exchange_name;
        if (auto v = (*a)["secret_name"].value<std::string>())
            ac.secret_name = *v;
        if (auto v = (*a)["testnet"].value<bool>())
            ac.testnet = *v;
        if (auto v = (*a)["rest_host"].value<std::string>())
            ac.rest_host = *v;
        if (auto v = (*a)["rest_port"].value<std::string>())
            ac.rest_port = *v;
        if (auto v = (*a)["ws_host"].value<std::string>())
            ac.ws_host = *v;
        if (auto v = (*a)["ws_port"].value<std::string>())
            ac.ws_port = *v;
        if (auto v = (*a)["ws_path"].value<std::string>())
            ac.ws_path = *v;
        if (auto v = (*a)["use_tls"].value<bool>())
            ac.use_tls = *v;
        if (auto v = (*a)["exec_queue_capacity"].value<int64_t>())
            ac.exec_queue_capacity = static_cast<uint32_t>(*v);
        if (auto* arr = (*a)["pinned_tls_sha256"].as_array()) {
            for (auto& elem : *arr)
                if (auto v = elem.value<std::string>())
                    ac.pinned_tls_sha256.push_back(*v);
        }
        if (auto v = (*a)["wallet_address"].value<std::string>())
            ac.wallet_address = *v;

        // Adapter connectivity must be fully specified; silently-empty
        // fields would surface as a cryptic connect-time crash later.
        // rest_host is required because every adapter also does REST
        // (snapshot fetch at minimum). Hyperliquid's action codec uses
        // ws_path, OKX/Binance/Deribit also do.
        if (ac.rest_host.empty() || ac.ws_host.empty() || ac.ws_port.empty() || ac.ws_path.empty())
            throw std::runtime_error(
                fmt::format("Adapter {} missing required rest_host/ws_host/ws_port/ws_path", exchange_name));

        // Fatal: prod environment with testnet adapters. Order-gateway
        // is the worst place for a flag mismatch — testnet=true in prod
        // means live trades go to the testnet venue.
        if (s.base.is_prod() && ac.testnet)
            throw std::runtime_error(
                fmt::format("environment = \"prod\" but adapter {} has testnet = true — refusing to start",
                            exchange_name));

        s.gateway.adapters.push_back(std::move(ac));
    }

    return s;
}

}  // namespace bpt::order_gateway::config

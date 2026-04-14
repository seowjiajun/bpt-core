#include "heimdall/config/settings.h"

#include <fmt/ranges.h>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <yggdrasil/logging_toml.h>

namespace heimdall::config {

namespace {

ygg::config::StreamConfig load_stream(const toml::table* t, std::string default_channel, int32_t default_stream_id) {
    ygg::config::StreamConfig s{std::move(default_channel), default_stream_id};
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

    if (auto v = root["environment"].value<std::string>())
        s.environment = *v;

    if (!s.environment.empty() && !exchange_config_path.empty()) {
        const bool path_has_live = exchange_config_path.find("live") != std::string::npos;
        const bool path_has_testnet = exchange_config_path.find("testnet") != std::string::npos;
        if ((s.environment == "prod" && path_has_testnet) ||
            ((s.environment == "qa" || s.environment == "dev") && path_has_live))
            ygg::log::warn("environment = \"{}\" but exchange_config = \"{}\" — possible misconfiguration",
                           s.environment,
                           exchange_config_path);
    }

    ygg::log::info("Environment: {}", s.environment.empty() ? "(not set)" : s.environment);

    // Read the exchanges filter from the instance config.
    std::unordered_set<std::string> exchange_filter;
    if (auto* arr = root["exchanges"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                exchange_filter.insert(*v);
        s.exchanges = {exchange_filter.begin(), exchange_filter.end()};
        ygg::log::info("Exchange filter: [{}]", fmt::join(s.exchanges, ", "));
    }

    if (auto* aeron = root["aeron"].as_table()) {
        if (auto v = (*aeron)["media_driver_dir"].value<std::string>())
            s.aeron.media_driver_dir = *v;
        s.aeron.order = load_stream((*aeron)["order"].as_table(), "aeron:ipc", 3001);
        s.aeron.exec_report = load_stream((*aeron)["exec_report"].as_table(), "aeron:ipc", 3002);
        s.aeron.heartbeat = load_stream((*aeron)["heartbeat"].as_table(), "aeron:ipc", 3003);
        s.aeron.account_snapshot = load_stream((*aeron)["account_snapshot"].as_table(), "aeron:ipc", 3004);
    }

    if (auto* g = root["heimdall"].as_table()) {
        if (auto v = (*g)["heartbeat_interval_ms"].value<int64_t>())
            s.heimdall.heartbeat_interval_ms = static_cast<uint32_t>(*v);
        if (auto v = (*g)["stale_order_timeout_ms"].value<int64_t>())
            s.heimdall.stale_order_timeout_ms = static_cast<uint32_t>(*v);

        if (auto* r = (*g)["risk"].as_table()) {
            if (auto v = (*r)["trading_enabled"].value<bool>())
                s.heimdall.risk.trading_enabled = *v;
            if (auto v = (*r)["max_order_size_usd"].value<double>())
                s.heimdall.risk.max_order_size_usd = *v;
            if (auto v = (*r)["max_notional_per_order_usd"].value<double>())
                s.heimdall.risk.max_notional_per_order_usd = *v;
            if (auto v = (*r)["max_open_orders_per_venue"].value<int64_t>())
                s.heimdall.risk.max_open_orders_per_venue = static_cast<uint32_t>(*v);
            if (auto v = (*r)["max_orders_per_second"].value<int64_t>())
                s.heimdall.risk.max_orders_per_second = static_cast<uint32_t>(*v);
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
        s.heimdall.adapters.push_back(std::move(ac));
    }

    if (auto* l = root["logging"].as_table())
        s.logging = ygg::logging::from_toml(*l);

    if (auto* m = root["metrics"].as_table())
        if (auto v = (*m)["port"].value<int64_t>())
            s.metrics_port = static_cast<uint16_t>(*v);

    return s;
}

}  // namespace heimdall::config

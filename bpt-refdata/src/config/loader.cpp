#include "refdata/config/settings.h"

#include <fmt/ranges.h>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <yggdrasil/logging_toml.h>

namespace bpt::refdata::config {

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
    ygg::log::info("Loading configuration from: {}", path);

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

    Settings settings;

    if (auto v = root["environment"].value<std::string>())
        settings.environment = *v;

    if (!settings.environment.empty() && !exchange_config_path.empty()) {
        const bool path_has_live = exchange_config_path.find("live") != std::string::npos;
        const bool path_has_testnet = exchange_config_path.find("testnet") != std::string::npos;
        if ((settings.environment == "prod" && path_has_testnet) ||
            ((settings.environment == "qa" || settings.environment == "dev") && path_has_live))
            ygg::log::warn("environment = \"{}\" but exchange_config = \"{}\" — possible misconfiguration",
                           settings.environment,
                           exchange_config_path);
    }

    ygg::log::info("Environment: {}", settings.environment.empty() ? "(not set)" : settings.environment);

    // Read the exchanges filter from the instance config.
    std::unordered_set<std::string> exchange_filter;
    if (auto* arr = root["exchanges"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                exchange_filter.insert(*v);
        settings.exchanges = {exchange_filter.begin(), exchange_filter.end()};
        ygg::log::info("Exchange filter: [{}]", fmt::join(settings.exchanges, ", "));
    }

    if (auto* aeron = root["aeron"].as_table()) {
        if (auto v = (*aeron)["media_driver_dir"].value<std::string>())
            settings.media_driver_dir = *v;
        settings.snapshot = load_stream((*aeron)["snapshot"].as_table(), "aeron:ipc", 1001);
        settings.delta = load_stream((*aeron)["delta"].as_table(), "aeron:ipc", 1002);
        settings.control = load_stream((*aeron)["control"].as_table(), "aeron:ipc", 1003);
        settings.fee_schedule = load_stream((*aeron)["fee_schedule"].as_table(), "aeron:ipc", 1004);
        settings.refdata_status = load_stream((*aeron)["refdata_status"].as_table(), "aeron:ipc", 1006);
        // Note: stream 1005 (funding_rate) has moved to Huginn — no longer loaded here
    }

    if (auto v = root["instrument_poll_interval_s"].value<int64_t>())
        settings.instrument_poll_interval_s = static_cast<uint32_t>(*v);

    if (auto* l = root["logging"].as_table())
        settings.logging = ygg::logging::from_toml(*l);

    if (auto* m = root["metrics"].as_table())
        if (auto v = (*m)["port"].value<int64_t>())
            settings.metrics_port = static_cast<uint16_t>(*v);

    if (auto* m = root["instrument_mapping"].as_table()) {
        if (auto v = (*m)["local_path"].value<std::string>())
            settings.instrument_mapping.local_path = *v;
        if (auto* s3 = (*m)["s3"].as_table()) {
            if (auto v = (*s3)["bucket"].value<std::string>())
                settings.instrument_mapping.s3.bucket = *v;
            if (auto v = (*s3)["region"].value<std::string>())
                settings.instrument_mapping.s3.region = *v;
            if (auto* keys = (*s3)["keys"].as_table()) {
                for (const auto& [exchange, key] : *keys) {
                    if (auto v = key.value<std::string>())
                        settings.instrument_mapping.s3.keys[std::string(exchange)] = *v;
                }
            }
        }
    }

    for (auto& elem : adapters_arr) {
        auto* a = elem.as_table();
        if (!a)
            continue;

        auto exchange_name = (*a)["exchange"].value<std::string>().value_or("");

        if (!exchange_filter.empty() && !exchange_filter.count(exchange_name))
            continue;

        AdapterConfig adapter;
        adapter.exchange = exchange_name;
        if (auto v = (*a)["secret_name"].value<std::string>())
            adapter.secret_name = *v;
        if (auto v = (*a)["enabled"].value<bool>())
            adapter.enabled = *v;
        if (auto v = (*a)["simulated"].value<bool>())
            adapter.simulated = *v;
        if (auto v = (*a)["rest_host"].value<std::string>())
            adapter.rest_host = *v;
        if (auto v = (*a)["rest_port"].value<std::string>())
            adapter.rest_port = *v;
        if (auto v = (*a)["ws_host"].value<std::string>())
            adapter.ws_host = *v;
        if (auto v = (*a)["ws_port"].value<std::string>())
            adapter.ws_port = *v;
        if (auto v = (*a)["use_tls"].value<bool>())
            adapter.use_tls = *v;
        settings.adapters.push_back(std::move(adapter));
    }

    return settings;
}

}  // namespace bpt::refdata::config

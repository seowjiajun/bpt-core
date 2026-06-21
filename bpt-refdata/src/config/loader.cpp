#include "refdata/config/settings.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/streams_map.h>
#include <bpt_common/config/profile_config.h>
#include <bpt_common/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <unordered_set>

namespace bpt::refdata::config {

Settings load(const std::string& path) {
    bpt::common::log::info("Loading configuration from: {}", path);

    toml::table root = toml::parse_file(path);

    // Deployment profile: shared file with environment/exchanges/exchange_config
    // that every service in the stack reads. Per-TOML fields below still win
    // when present — operators can override for one-off runs.
    bpt::common::config::ProfileConfig profile;
    bool have_profile = false;
    if (auto v = root["profile_config"].value<std::string>()) {
        profile = bpt::common::config::load_profile_config(*v);
        have_profile = true;
        bpt::common::log::info("Loaded deployment profile from {} (env={}, exchanges=[{}])",
                               *v,
                               bpt::common::to_string(profile.environment),
                               fmt::join(profile.exchanges, ", "));
        // load_base_settings reads `environment` from the toml::table — inject
        // the profile value if the per-TOML doesn't supply one.
        if (!root.contains("environment"))
            root.insert("environment", std::string(bpt::common::to_string(profile.environment)));
    }

    // Load raw adapter list from exchange config file, falling back to inline.
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

    Settings settings;
    bpt::app::load_base_settings(root, settings.base);

    if (!exchange_config_path.empty()) {
        const bool path_has_live = exchange_config_path.find("live") != std::string::npos;
        const bool path_has_testnet = exchange_config_path.find("testnet") != std::string::npos;
        if (settings.base.is_prod() && path_has_testnet)
            throw std::runtime_error(
                fmt::format("environment = \"prod\" but exchange_config = \"{}\" resolves to a testnet path — "
                            "refusing to start",
                            exchange_config_path));
        if (!settings.base.is_prod() && path_has_live)
            bpt::common::log::warn("environment = \"{}\" but exchange_config = \"{}\" — possible misconfiguration",
                                   bpt::app::to_string(settings.base.environment),
                                   exchange_config_path);
    }

    bpt::common::log::info("Environment: {}", bpt::app::to_string(settings.base.environment));

    // Read the exchanges filter — per-TOML override wins over profile.
    std::unordered_set<std::string> exchange_filter;
    if (auto* arr = root["exchanges"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                exchange_filter.insert(*v);
        settings.exchanges = {exchange_filter.begin(), exchange_filter.end()};
    } else if (have_profile) {
        for (const auto& ex : profile.exchanges)
            exchange_filter.insert(ex);
        settings.exchanges = profile.exchanges;
    }
    if (!settings.exchanges.empty())
        bpt::common::log::info("Exchange filter: [{}]", fmt::join(settings.exchanges, ", "));

    // Layer 1: shared aeron stream registry. If aeron_config points at a
    // streams.toml, every stream comes from there; otherwise we fall back
    // to the hardcoded ids below (which match the historical block layout).
    bpt::common::config::AeronStreamMap shared_streams;
    if (auto v = root["aeron_config"].value<std::string>()) {
        shared_streams = bpt::common::config::load_shared_streams(*v);
        bpt::common::log::info("Loaded shared aeron stream map from {} ({} streams)",
                               *v,
                               shared_streams.stream_ids.size());
        // streams.toml is the source of truth for media_driver_dir too —
        // overrides whatever bpt::app::load_base_settings populated.
        if (!shared_streams.media_driver_dir.empty())
            settings.base.media_driver_dir = shared_streams.media_driver_dir;
    }

    using bpt::common::config::resolve_stream;
    settings.refdata_snapshot = resolve_stream(shared_streams, "refdata.snapshot", 1001);
    settings.refdata_delta = resolve_stream(shared_streams, "refdata.delta", 1002);
    settings.refdata_control = resolve_stream(shared_streams, "refdata.control", 1003);
    settings.fee_schedule = resolve_stream(shared_streams, "refdata.fee_schedule", 1004);
    settings.refdata_status = resolve_stream(shared_streams, "refdata.status", 1006);
    // Note: stream 2005 (funding_rate) is published by MdGateway, not consumed here.

    if (auto v = root["instrument_poll_interval_s"].value<int64_t>())
        settings.instrument_poll_interval_s = static_cast<uint32_t>(*v);

    if (auto* m = root["instrument_mapping"].as_table()) {
        if (auto v = (*m)["local_path"].value<std::string>())
            settings.instrument_mapping.local_path = *v;
        if (auto* srcs = (*m)["sources"].as_table()) {
            for (const auto& [exchange, path] : *srcs) {
                if (auto v = path.value<std::string>())
                    settings.instrument_mapping.sources.paths[std::string(exchange)] = *v;
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
        if (auto* arr = (*a)["pinned_tls_sha256"].as_array()) {
            for (auto& elem : *arr)
                if (auto v = elem.value<std::string>())
                    adapter.pinned_tls_sha256.push_back(*v);
        }

        // Only validate connectivity for adapters that are actually
        // going to run. `enabled = false` is a legal way to declare an
        // adapter in TOML without wiring it up (e.g. while rolling out
        // a new venue); skip the required-field check in that case.
        if (adapter.enabled) {
            if (adapter.rest_host.empty() || adapter.ws_host.empty() || adapter.ws_port.empty())
                throw std::runtime_error(
                    fmt::format("Adapter {} missing required rest_host/ws_host/ws_port", exchange_name));
        }

        settings.adapters.push_back(std::move(adapter));
    }

    return settings;
}

}  // namespace bpt::refdata::config

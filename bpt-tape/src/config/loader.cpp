#include "tape/config/settings.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <bpt_app/base_settings.h>
#include <bpt_common/logging.h>

namespace bpt::tape::config {

namespace {

// Fill an AdapterConfig from a TOML table, mirroring the subset of fields
// bpt-md-gateway's loader populates. Recorder doesn't need every knob;
// keep this in sync with what AdapterBase + each venue adapter actually
// reads. Validation/breaker fields are left at defaults — recorder
// instances don't enforce price-deviation checks.
bpt::md_gateway::config::AdapterConfig parse_adapter(const toml::table& t) {
    bpt::md_gateway::config::AdapterConfig ac;
    if (auto v = t["exchange"].value<std::string>()) ac.exchange = *v;
    if (auto v = t["ws_host"].value<std::string>()) ac.ws_host = *v;
    if (auto v = t["ws_port"].value<std::string>()) ac.ws_port = *v;
    if (auto v = t["ws_path"].value<std::string>()) ac.ws_path = *v;
    if (auto v = t["use_tls"].value<bool>()) ac.use_tls = *v;
    if (auto v = t["ws_connect_timeout_ms"].value<int64_t>())
        ac.ws_connect_timeout_ms = static_cast<uint32_t>(*v);
    if (auto v = t["ws_ping_interval_ms"].value<int64_t>())
        ac.ws_ping_interval_ms = static_cast<uint32_t>(*v);
    if (auto v = t["ws_read_timeout_ms"].value<int64_t>())
        ac.ws_read_timeout_ms = static_cast<uint32_t>(*v);
    if (auto v = t["ws_liveness_timeout_ms"].value<int64_t>())
        ac.ws_liveness_timeout_ms = static_cast<uint32_t>(*v);
    if (auto v = t["max_price_deviation_pct"].value<double>())
        ac.max_price_deviation_pct = *v;
    if (auto v = t["io_thread_cpu"].value<int64_t>())
        ac.io_thread_cpu = static_cast<int>(*v);
    if (auto v = t["so_rcvbuf_bytes"].value<int64_t>())
        ac.so_rcvbuf_bytes = static_cast<uint32_t>(*v);
    return ac;
}

}  // namespace

Settings load(const std::string& path) {
    toml::table root = toml::parse_file(path);

    Settings s;
    bpt::app::load_base_settings(root, s.base);

    bpt::common::log::info("Environment: {}", bpt::app::to_string(s.base.environment));

    // Adapter list: either inline [[adapters]] or via exchange_config indirection.
    toml::array adapters_arr;
    if (auto v = root["exchange_config"].value<std::string>()) {
        toml::table exchange = toml::parse_file(*v);
        if (auto* arr = exchange["adapters"].as_array())
            adapters_arr = *arr;
    } else if (auto* arr = root["adapters"].as_array()) {
        adapters_arr = *arr;
    } else {
        throw std::runtime_error("bpt-tape config: missing [adapters] or exchange_config");
    }

    // Filter by recording_universe_venues if non-empty.
    std::unordered_set<std::string> venue_filter;
    if (auto* arr = root["recording_universe_venues"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                venue_filter.insert(*v);
        s.recording_universe_venues = {venue_filter.begin(), venue_filter.end()};
    }

    for (auto& elem : adapters_arr) {
        auto* a = elem.as_table();
        if (!a) continue;
        auto ac = parse_adapter(*a);
        if (!venue_filter.empty() && !venue_filter.count(ac.exchange))
            continue;
        s.mdgw_adapters.push_back(std::move(ac));
    }
    bpt::common::log::info("bpt-tape adapters: {} venues",
                           s.mdgw_adapters.size());

    if (auto v = root["instrument_mapping_path"].value<std::string>())
        s.instrument_mapping_path = *v;

    if (auto* uf = root["universe_filter"].as_table()) {
        if (auto* arr = (*uf)["inst_types"].as_array()) {
            for (auto& elem : *arr)
                if (auto v = elem.value<std::string>())
                    s.universe_filter.inst_types.push_back(*v);
        }
        if (auto* arr = (*uf)["exclude_bases"].as_array()) {
            for (auto& elem : *arr)
                if (auto v = elem.value<std::string>())
                    s.universe_filter.exclude_bases.push_back(*v);
        }
        if (auto v = (*uf)["default_depth"].value<int64_t>())
            s.universe_filter.default_depth = static_cast<uint8_t>(*v);
    }
    bpt::common::log::info("bpt-tape universe filter: types=[{}] exclude=[{}] depth={}",
                           fmt::join(s.universe_filter.inst_types, ","),
                           fmt::join(s.universe_filter.exclude_bases, ","),
                           s.universe_filter.default_depth);

    if (auto* r = root["recording"].as_table()) {
        if (auto v = (*r)["output_dir"].value<std::string>())
            s.recording.output_dir = *v;
        if (auto v = (*r)["rotate_interval_seconds"].value<int64_t>())
            s.recording.rotate_interval_seconds = static_cast<uint32_t>(*v);
        if (auto v = (*r)["fsync_interval_ms"].value<int64_t>())
            s.recording.fsync_interval_ms = static_cast<uint32_t>(*v);
        if (auto v = (*r)["buffer_bytes"].value<int64_t>())
            s.recording.buffer_bytes = static_cast<uint32_t>(*v);
    }

    if (auto* m = root["metrics"].as_table()) {
        if (auto v = (*m)["host"].value<std::string>())
            s.metrics_host = *v;
    }

    if (auto* arr = root["refdata_endpoints"].as_array()) {
        for (auto& elem : *arr) {
            auto* t = elem.as_table();
            if (!t) continue;
            RefdataEndpoint ep;
            if (auto v = (*t)["exchange"].value<std::string>()) ep.exchange = *v;
            if (auto v = (*t)["host"].value<std::string>()) ep.host = *v;
            if (auto v = (*t)["port"].value<std::string>()) ep.port = *v;
            if (auto v = (*t)["use_tls"].value<bool>()) ep.use_tls = *v;
            if (auto v = (*t)["method"].value<std::string>()) ep.method = *v;
            if (auto v = (*t)["path"].value<std::string>()) ep.path = *v;
            if (auto v = (*t)["body"].value<std::string>()) ep.body = *v;
            if (auto v = (*t)["interval_seconds"].value<int64_t>())
                ep.interval_seconds = static_cast<uint32_t>(*v);
            if (ep.exchange.empty() || ep.host.empty() || ep.path.empty()) {
                throw std::runtime_error(
                    "bpt-tape config: [[refdata_endpoints]] entry missing exchange/host/path");
            }
            // Honour recording_universe_venues if it's set — keeps the
            // recorder host single-purpose to a venue subset.
            if (!venue_filter.empty() && !venue_filter.count(ep.exchange))
                continue;
            s.refdata_endpoints.push_back(std::move(ep));
        }
    }
    bpt::common::log::info("bpt-tape refdata_endpoints: {} entries",
                           s.refdata_endpoints.size());

    return s;
}

}  // namespace bpt::tape::config

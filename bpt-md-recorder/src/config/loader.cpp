#include "md_recorder/config/settings.h"

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <toml++/toml.hpp>
#include <unordered_set>
#include <bpt_app/base_settings.h>
#include <bpt_common/logging.h>

namespace bpt::md_recorder::config {

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
        throw std::runtime_error("md-recorder config: missing [adapters] or exchange_config");
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
    bpt::common::log::info("md-recorder adapters: {} venues",
                           s.mdgw_adapters.size());

    if (auto* arr = root["universe"].as_array()) {
        for (auto& elem : *arr) {
            auto* t = elem.as_table();
            if (!t) continue;
            UniverseEntry u;
            if (auto v = (*t)["instrument_id"].value<int64_t>())
                u.instrument_id = static_cast<uint64_t>(*v);
            if (auto v = (*t)["venue"].value<std::string>()) u.venue = *v;
            if (auto v = (*t)["symbol"].value<std::string>()) u.symbol = *v;
            if (auto v = (*t)["depth"].value<int64_t>())
                u.depth = static_cast<uint8_t>(*v);
            if (u.instrument_id == 0 || u.venue.empty() || u.symbol.empty())
                throw std::runtime_error(
                    "[[universe]] entry missing instrument_id, venue, or symbol");
            if (!venue_filter.empty() && !venue_filter.count(u.venue))
                continue;
            s.universe.push_back(std::move(u));
        }
    }
    bpt::common::log::info("md-recorder universe: {} entries", s.universe.size());

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

    return s;
}

}  // namespace bpt::md_recorder::config

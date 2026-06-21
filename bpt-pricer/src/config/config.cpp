#include "pricer/config/settings.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/streams_map.h>
#include <bpt_common/config/profile_config.h>
#include <bpt_common/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <toml++/toml.hpp>

namespace bpt::pricer::config {

Settings load(const std::string& path) {
    Settings s;
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

    bpt::app::load_base_settings(root, s.base);

    bpt::common::config::AeronStreamMap shared_streams;
    if (auto v = root["aeron_config"].value<std::string>()) {
        shared_streams = bpt::common::config::load_shared_streams(*v);
        bpt::common::log::info("Loaded shared aeron stream map from {} ({} streams)",
                               *v,
                               shared_streams.stream_ids.size());
        if (!shared_streams.media_driver_dir.empty())
            s.base.media_driver_dir = shared_streams.media_driver_dir;
    }

    using bpt::common::config::resolve_stream;
    s.aeron.md.data = resolve_stream(shared_streams, "md.feed", 2002);
    s.aeron.md.control = resolve_stream(shared_streams, "md.control", 2001);
    s.aeron.refdata.snapshot = resolve_stream(shared_streams, "refdata.snapshot", 1001);
    s.aeron.refdata.delta = resolve_stream(shared_streams, "refdata.delta", 1002);
    s.aeron.refdata.control = resolve_stream(shared_streams, "refdata.control", 1003);
    s.aeron.vol_surface = resolve_stream(shared_streams, "pricer.vol_surface", 4001);
    s.aeron.pricer_status = resolve_stream(shared_streams, "pricer.status", 4002);

    if (auto* arr = root["exchanges"].as_array()) {
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                s.exchanges.push_back(*v);
    } else if (have_profile) {
        s.exchanges = profile.exchanges;
    }

    if (auto* arr = root["underlyings"].as_array())
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                s.underlyings.push_back(*v);

    if (auto v = root["publish_interval_ms"].value<int64_t>())
        s.publish_interval_ms = static_cast<uint32_t>(*v);
    if (auto v = root["risk_free_rate"].value<double>())
        s.risk_free_rate = *v;
    if (auto v = root["newton_max_iterations"].value<int64_t>())
        s.newton_max_iterations = static_cast<uint32_t>(*v);
    if (auto v = root["newton_tolerance"].value<double>())
        s.newton_tolerance = *v;

    if (auto t = root["universe"].as_table()) {
        if (auto v = (*t)["front_n_expiries"].value<int64_t>())
            s.universe.front_n_expiries = static_cast<uint32_t>(*v);
        if (auto v = (*t)["max_strikes_per_expiry"].value<int64_t>())
            s.universe.max_strikes_per_expiry = static_cast<uint32_t>(*v);
    }

    return s;
}

}  // namespace bpt::pricer::config

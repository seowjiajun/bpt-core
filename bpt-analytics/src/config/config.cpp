#include "analytics/config/settings.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/streams_map.h>
#include <bpt_common/config/profile_config.h>
#include <bpt_common/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <toml++/toml.hpp>

namespace bpt::analytics::config {

Settings load(const std::string& path) {
    auto tbl = toml::parse_file(path);
    Settings s;

    if (auto v = tbl["profile_config"].value<std::string>()) {
        auto profile = bpt::common::config::load_profile_config(*v);
        bpt::common::log::info("Loaded deployment profile from {} (env={}, exchanges=[{}])",
                               *v,
                               bpt::common::to_string(profile.environment),
                               fmt::join(profile.exchanges, ", "));
        if (!tbl.contains("environment"))
            tbl.insert("environment", std::string(bpt::common::to_string(profile.environment)));
    }

    bpt::app::load_base_settings(tbl, s.base);

    bpt::common::config::AeronStreamMap shared_streams;
    if (auto v = tbl["aeron_config"].value<std::string>()) {
        shared_streams = bpt::common::config::load_shared_streams(*v);
        bpt::common::log::info("Loaded shared aeron stream map from {} ({} streams)",
                               *v, shared_streams.stream_ids.size());
        if (!shared_streams.media_driver_dir.empty())
            s.base.media_driver_dir = shared_streams.media_driver_dir;
    }

    using bpt::common::config::resolve_stream;
    s.exec_report = resolve_stream(shared_streams, "exec_report", 3002);
    s.md_data     = resolve_stream(shared_streams, "md_data",     2002);
    s.toxicity    = resolve_stream(shared_streams, "toxicity",    5001);

    if (auto t = tbl["tyr"]) {
        s.markout_max_pending = t["markout_max_pending"].value_or(std::size_t{64});
        s.scorer_window_size = t["scorer_window_size"].value_or(std::size_t{50});
        s.scorer_window_duration_ns = t["scorer_window_duration_ns"].value_or(uint64_t{0});
        s.scorer_min_samples = t["scorer_min_samples"].value_or(std::size_t{5});
        s.publish_interval_ms = t["publish_interval_ms"].value_or(uint32_t{5000});
    }

    return s;
}

}  // namespace bpt::analytics::config

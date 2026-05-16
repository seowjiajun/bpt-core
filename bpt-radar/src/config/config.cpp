#include "radar/config/settings.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/streams_map.h>
#include <bpt_common/config/profile_config.h>
#include <bpt_common/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <toml++/toml.hpp>

namespace bpt::radar::config {

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
                               *v,
                               shared_streams.stream_ids.size());
        if (!shared_streams.media_driver_dir.empty())
            s.base.media_driver_dir = shared_streams.media_driver_dir;
    }

    using bpt::common::config::resolve_stream;
    s.vol_surface = resolve_stream(shared_streams, "vol_surface", 4001);
    s.instrument_stats = resolve_stream(shared_streams, "instrument_stats", 2004);
    s.funding_rate = resolve_stream(shared_streams, "funding_rate", 1005);
    s.refdata_snapshot = resolve_stream(shared_streams, "refdata_snapshot", 1001);
    s.md_data = resolve_stream(shared_streams, "md_data", 2002);
    s.market_color = resolve_stream(shared_streams, "market_color", 6002);

    if (auto t = tbl["radar"])
        s.publish_interval_ms = t["publish_interval_ms"].value_or(uint32_t{2000});

    return s;
}

}  // namespace bpt::radar::config

#include "analytics/config/settings.h"

#include <toml++/toml.hpp>
#include <bpt_app/base_settings.h>

namespace bpt::analytics::config {

Settings load(const std::string& path) {
    auto tbl = toml::parse_file(path);
    Settings s;
    bpt::app::load_base_settings(tbl, s.base);

    auto aeron = [&](const char* section) -> bpt::common::config::StreamConfig {
        auto node = tbl["aeron"][section];
        return {
            .channel = node["channel"].value_or(std::string{"aeron:ipc"}),
            .stream_id = node["stream_id"].value_or(0),
        };
    };

    s.exec_report = aeron("exec_report");
    s.md_data = aeron("md_data");
    s.toxicity = aeron("toxicity");

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

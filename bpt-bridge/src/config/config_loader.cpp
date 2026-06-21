#include "bridge/config/settings.h"

#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/streams_map.h>
#include <bpt_common/config/profile_config.h>
#include <bpt_common/logging.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace bpt::bridge::config {

Settings load(const std::string& path, const std::string& profile_override) {
    Settings s;

    // Give base.media_driver_dir a bridge-specific default. The shared
    // streams.toml may override; load_base_settings reads [aeron].media_driver_dir
    // for the no-shared-file path.
    s.base.media_driver_dir = "/dev/shm/aeron-bpt";

    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(
            fmt::format("Failed to load bridge config {}: {}", path, std::string(e.description())));
    }

    // Profile path: CLI --profile wins, else read profile_config from TOML.
    std::string profile_path = profile_override;
    if (profile_path.empty()) {
        if (auto v = root["profile_config"].value<std::string>())
            profile_path = *v;
    }
    if (!profile_path.empty()) {
        auto profile = bpt::common::config::load_profile_config(profile_path);
        bpt::common::log::info("Loaded deployment profile from {} (env={}, exchanges=[{}])",
                               profile_path,
                               bpt::common::to_string(profile.environment),
                               fmt::join(profile.exchanges, ", "));
        // Profile wins over the TOML's hardcoded `environment` — that's the
        // point of plumbing --profile through systemd, so prod-* unit files
        // make bridge log "prod" instead of "qa".
        root.insert_or_assign("environment", std::string(bpt::common::to_string(profile.environment)));
    } else if (!root.contains("environment")) {
        // No profile and no `environment` in the TOML — bridge.live.toml
        // deliberately drops `environment` because the profile owns it.
        // If we got here, the operator forgot BPT_BRIDGE_PROFILE in the env
        // file (or removed --profile from the unit). load_base_settings
        // would throw cryptically; surface the real cause instead.
        throw std::runtime_error(
            "bridge: no `profile_config` in TOML and no --profile CLI arg — "
            "expected BPT_BRIDGE_PROFILE to be set in the active env file");
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
    s.md_data = resolve_stream(shared_streams, "md.feed", 2002);
    s.exec_report = resolve_stream(shared_streams, "order.exec_report", 3002);
    s.console_control = resolve_stream(shared_streams, "bridge.console_control", 9003);
    s.portfolio = resolve_stream(shared_streams, "bridge.portfolio", 9004);
    s.account_snapshot = resolve_stream(shared_streams, "order.account_snapshot", 3004);
    s.toxicity = resolve_stream(shared_streams, "analytics.toxicity", 0);
    s.market_color = resolve_stream(shared_streams, "radar.market_color", 0);

    // WebSocket
    if (auto* ws = root["ws"].as_table()) {
        if (auto v = (*ws)["port"].value<int64_t>())
            s.ws_port = static_cast<uint16_t>(*v);
    }

    // Session
    if (auto* sess = root["session"].as_table()) {
        if (auto v = (*sess)["symbol"].value<std::string>())
            s.symbol = *v;
        if (auto v = (*sess)["strategy"].value<std::string>())
            s.strategy = *v;
        if (auto v = (*sess)["exchange"].value<std::string>())
            s.exchange = *v;
        if (auto v = (*sess)["mode"].value<std::string>())
            s.mode = *v;
        if (auto v = (*sess)["instrument_type"].value<std::string>())
            s.instrument_type = *v;
        if (auto v = (*sess)["instrument_id"].value<int64_t>())
            s.instrument_id = static_cast<uint64_t>(*v);
    }

    return s;
}

}  // namespace bpt::bridge::config

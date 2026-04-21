#include "bridge/settings.h"

#include <bpt_app/base_settings.h>
#include <fmt/format.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace bridge::config {

namespace {

bpt::common::config::StreamConfig load_stream(const toml::table* t,
                                              std::string default_channel,
                                              int32_t default_stream_id) {
    bpt::common::config::StreamConfig s{std::move(default_channel), default_stream_id};
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
    Settings s;

    // Give base.media_driver_dir a bridge-specific default (matches the
    // previous behaviour where Settings::media_driver_dir defaulted to
    // /dev/shm/aeron-bpt). load_base_settings only overrides from
    // [aeron].media_driver_dir, so the pre-set default survives unless
    // the TOML says otherwise.
    s.base.media_driver_dir = "/dev/shm/aeron-bpt";

    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(
            fmt::format("Failed to load bridge config {}: {}", path, std::string(e.description())));
    }

    bpt::app::load_base_settings(root, s.base);

    // Aeron stream IDs (media_driver_dir loaded above).
    if (auto* aeron = root["aeron"].as_table()) {
        s.md_data = load_stream((*aeron)["md_data"].as_table(), s.md_data.channel, s.md_data.stream_id);
        s.exec_report = load_stream((*aeron)["exec_report"].as_table(), s.exec_report.channel, s.exec_report.stream_id);
        s.control_command =
            load_stream((*aeron)["control_command"].as_table(), s.control_command.channel, s.control_command.stream_id);
        s.portfolio_snapshot = load_stream((*aeron)["portfolio_snapshot"].as_table(),
                                           s.portfolio_snapshot.channel,
                                           s.portfolio_snapshot.stream_id);
        s.account_snapshot = load_stream((*aeron)["account_snapshot"].as_table(),
                                         s.account_snapshot.channel,
                                         s.account_snapshot.stream_id);
        s.toxicity = load_stream((*aeron)["toxicity"].as_table(), s.toxicity.channel, s.toxicity.stream_id);
    }

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

}  // namespace bridge::config

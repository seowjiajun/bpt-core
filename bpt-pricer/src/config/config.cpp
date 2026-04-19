#include "pricer/config/settings.h"

#include <toml++/toml.hpp>

namespace bpt::pricer::config {

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
    Settings s;
    toml::table root = toml::parse_file(path);

    s.media_driver_dir = root["media_driver_dir"].value<std::string>().value_or("");
    if (auto* aeron = root["aeron"].as_table())
        s.media_driver_dir = (*aeron)["media_driver_dir"].value<std::string>().value_or(s.media_driver_dir);

    s.md_input = load_stream(root["md_input"].as_table(), "aeron:ipc", 2002);
    s.vol_surface = load_stream(root["vol_surface"].as_table(), "aeron:ipc", 4001);
    s.status = load_stream(root["status"].as_table(), "aeron:ipc", 4002);

    if (auto* rd = root["refdata"].as_table()) {
        s.refdata_snapshot = load_stream((*rd)["snapshot"].as_table(), "aeron:ipc", 1001);
        s.refdata_delta = load_stream((*rd)["delta"].as_table(), "aeron:ipc", 1002);
        s.refdata_control = load_stream((*rd)["control"].as_table(), "aeron:ipc", 1003);
    }

    if (auto* arr = root["exchanges"].as_array())
        for (auto& elem : *arr)
            if (auto v = elem.value<std::string>())
                s.exchanges.push_back(*v);

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
    if (auto* l = root["logging"].as_table()) {
        if (auto v = (*l)["level"].value<std::string>())
            s.logging.level = *v;
        if (auto v = (*l)["dir"].value<std::string>())
            s.logging.dir = *v;
    }

    if (auto v = root["metrics_port"].value<int64_t>())
        s.metrics_port = static_cast<uint16_t>(*v);

    return s;
}

}  // namespace bpt::pricer::config

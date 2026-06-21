#include "bpt_common/aeron/streams_map.h"

#include "bpt_common/logging.h"

#include <fmt/format.h>
#include <stdexcept>
#include <toml++/toml.hpp>

namespace bpt::common::config {

AeronStreamMap load_shared_streams(const std::string& path) {
    AeronStreamMap out;
    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error(fmt::format("aeron_config: failed to parse '{}': {}", path, e.description()));
    }

    if (auto* aeron = root["aeron"].as_table()) {
        if (auto v = (*aeron)["media_driver_dir"].value<std::string>())
            out.media_driver_dir = *v;
    }

    // [streams] accepts both flat (`name = id`) and nested
    // (`[streams.<group>]` then `leaf = id` → global name "group.leaf")
    // entries. Nested groups streams by producing service; the resolved
    // global name is the dotted path. Flat is kept for back-compat.
    if (auto* streams = root["streams"].as_table()) {
        for (const auto& [k, v] : *streams) {
            if (auto id = v.value<int64_t>()) {
                out.stream_ids.emplace(std::string(k.str()), static_cast<int32_t>(*id));
            } else if (auto* group = v.as_table()) {
                const std::string prefix(k.str());
                for (const auto& [leaf, lv] : *group) {
                    if (auto lid = lv.value<int64_t>())
                        out.stream_ids.emplace(prefix + "." + std::string(leaf.str()), static_cast<int32_t>(*lid));
                }
            }
        }
    }

    // [channels] mirrors the same shape — flat dotted key or nested group.
    if (auto* channels = root["channels"].as_table()) {
        for (const auto& [k, v] : *channels) {
            if (auto c = v.value<std::string>()) {
                out.channels.emplace(std::string(k.str()), *c);
            } else if (auto* group = v.as_table()) {
                const std::string prefix(k.str());
                for (const auto& [leaf, lv] : *group) {
                    if (auto lc = lv.value<std::string>())
                        out.channels.emplace(prefix + "." + std::string(leaf.str()), *lc);
                }
            }
        }
    }

    return out;
}

StreamConfig resolve_stream(const AeronStreamMap& shared,
                            std::string_view global_name,
                            int32_t fallback_id,
                            std::string_view fallback_channel) {
    StreamConfig s{std::string(fallback_channel), fallback_id};

    if (auto it = shared.stream_ids.find(std::string(global_name)); it != shared.stream_ids.end()) {
        s.stream_id = it->second;
    } else if (!shared.stream_ids.empty()) {
        // Shared file was loaded but didn't contain this name — likely a
        // missing entry the operator forgot to add. Loud but non-fatal.
        bpt::common::log::warn("aeron stream '{}' missing from shared streams.toml — using fallback id={}",
                               global_name,
                               fallback_id);
    }
    // shared.stream_ids.empty() == "aeron_config wasn't set on this service" — silent fallback.

    if (auto it = shared.channels.find(std::string(global_name)); it != shared.channels.end()) {
        s.channel = it->second;
    }
    return s;
}

}  // namespace bpt::common::config

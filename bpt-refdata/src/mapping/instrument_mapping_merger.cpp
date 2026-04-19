#include "refdata/mapping/instrument_mapping_merger.h"

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <bpt_common/logging.h>

namespace bpt::refdata::mapping {

using json = nlohmann::json;

namespace {

// forward: union (src keys overwrite on collision — should never happen for
// different exchanges by construction).
// reverse: entries merged by unioning exchanges dicts; base/quote/type from
// first seen (they're identical across sources for the same canonical id).
void merge_into(json& dst, const json& src) {
    if (src.contains("forward") && src["forward"].is_object()) {
        for (const auto& [key, val] : src["forward"].items())
            dst["forward"][key] = val;
    }

    if (src.contains("reverse") && src["reverse"].is_object()) {
        for (const auto& [cid, entry] : src["reverse"].items()) {
            if (!dst["reverse"].contains(cid)) {
                dst["reverse"][cid] = entry;
            } else if (entry.contains("exchanges") && entry["exchanges"].is_object()) {
                for (const auto& [ex_id, sym] : entry["exchanges"].items())
                    dst["reverse"][cid]["exchanges"][ex_id] = sym;
            }
        }
    }

    const uint64_t src_ts = src.value("exported_at", uint64_t{0});
    const uint64_t dst_ts = dst.value("exported_at", uint64_t{0});
    if (src_ts > dst_ts)
        dst["exported_at"] = src_ts;
}

}  // namespace

InstrumentMappingMerger::InstrumentMappingMerger(const Config& cfg) : cfg_(cfg) {}

bool InstrumentMappingMerger::merge(const std::string& local_path) const {
    if (cfg_.sources.empty()) {
        bpt::common::log::error("[InstrumentMappingMerger] No sources configured");
        return false;
    }

    json merged;
    merged["forward"] = json::object();
    merged["reverse"] = json::object();
    merged["exported_at"] = uint64_t{0};

    for (const auto& [exchange_name, src_path] : cfg_.sources) {
        std::ifstream in(src_path);
        if (!in) {
            bpt::common::log::error("[InstrumentMappingMerger] Cannot open source for {}: {}", exchange_name, src_path);
            return false;
        }

        std::ostringstream body_ss;
        body_ss << in.rdbuf();
        const std::string body = body_ss.str();

        json parsed;
        try {
            parsed = json::parse(body);
        } catch (const json::exception& e) {
            bpt::common::log::error("[InstrumentMappingMerger] JSON parse error for {} ({}): {}",
                            exchange_name, src_path, e.what());
            return false;
        }

        merge_into(merged, parsed);
        bpt::common::log::info("[InstrumentMappingMerger] Loaded {} from {} ({} bytes)",
                       exchange_name, src_path, body.size());
    }

    merged["instrument_count"] = merged["reverse"].size();

    const std::string serialised = merged.dump();
    const std::string tmp_path = local_path + ".tmp";

    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            bpt::common::log::error("[InstrumentMappingMerger] Cannot open tmp file for writing: {}", tmp_path);
            return false;
        }
        out.write(serialised.data(), static_cast<std::streamsize>(serialised.size()));
        if (!out) {
            bpt::common::log::error("[InstrumentMappingMerger] Write to tmp file failed: {}", tmp_path);
            return false;
        }
    }

    if (std::rename(tmp_path.c_str(), local_path.c_str()) != 0) {
        bpt::common::log::error("[InstrumentMappingMerger] Atomic rename failed: {} → {}", tmp_path, local_path);
        return false;
    }

    bpt::common::log::info("[InstrumentMappingMerger] Merged {} exchange(s) → {} ({} instruments, {} bytes)",
                   cfg_.sources.size(), local_path, merged["reverse"].size(), serialised.size());
    return true;
}

}  // namespace bpt::refdata::mapping

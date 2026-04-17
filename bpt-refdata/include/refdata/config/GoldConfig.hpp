#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace muninn::config {

struct GoldConfig {
    std::string base_path;
    // Filename written by data-forge gold_writer (default: gold.ndjson).
    std::string snapshot_filename = "gold.ndjson";
    // Dataset sub-path in the GCS layout: {base}/{venue}/{dataset}/{date}/{filename}.
    std::string snapshot_dataset = "instruments";
    // Optional per-venue path overrides. If absent, path is computed at runtime.
    std::map<std::string, std::string> venue_paths;

    std::vector<std::string> enabled_inst_types;
    std::vector<std::string> allow_symbols;
    std::vector<std::string> deny_symbols;
};

}  // namespace muninn::config

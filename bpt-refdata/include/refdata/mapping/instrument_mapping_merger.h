#pragma once

#include <map>
#include <string>

namespace bpt::refdata::mapping {

// Merges per-exchange instrument mapping files produced by the data-forge
// pipeline into a single canonical file. Source files are plain JSON on the
// local filesystem — typically under config/generated/ and shipped with the
// deploy tarball after a data-forge PR has been merged.
//
// Each source file has the same shape:
//   { "forward": {...}, "reverse": {...}, "exported_at": <unix-ms> }
// The merger takes the union of forward/reverse entries across exchanges and
// writes the merged result atomically to local_path (via .tmp + rename).
class InstrumentMappingMerger {
public:
    struct Config {
        // exchange_name (lowercase) → source file path. Relative paths are
        // resolved against the service's working directory.
        std::map<std::string, std::string> sources;
    };

    explicit InstrumentMappingMerger(const Config& cfg);

    // Reads every configured source file, merges them in memory, writes the
    // merged mapping atomically to local_path. Returns true on success.
    [[nodiscard]] bool merge(const std::string& local_path) const;

private:
    Config cfg_;
};

}  // namespace bpt::refdata::mapping

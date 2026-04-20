#pragma once

// topology.h — Central CPU-affinity topology loader.
//
// Single source of truth for which threads pin to which cores on this
// host. Each service looks up its own roles by string name and pins
// its hot threads accordingly. A missing role = unpinned (service logs
// a warning). An empty topology = everything unpinned (dev-laptop safe).
//
// Config shape (TOML):
//   [host]
//   total_cores  = 16        # informational; validated against nproc
//
//   [[assignment]]
//   role = "mdgw.okx.io"
//   core = 4
//   [[assignment]]
//   role = "ogw.okx.io"
//   core = 5
//
// Validation at load time:
//   - Every core must be < sysconf(_SC_NPROCESSORS_ONLN)
//   - No two assignments may claim the same core
//   - No two assignments may reuse the same role name
//   - host.total_cores (if present) must match actual nproc
//
// The topology is read-only after load; lookups are O(1) hash-map hits.

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

namespace bpt::common::util {

class Topology {
public:
    // Empty topology — all lookups miss, used as the "no pinning"
    // default when services are started without a topology path.
    Topology() = default;

    // Load from TOML file at `path`. If path is empty, returns an empty
    // Topology (every pin request will fall through as unpinned). Throws
    // std::runtime_error on parse failure, unknown nproc, or any
    // validation rule violation (duplicate core / role, core >= nproc,
    // host.total_cores mismatch).
    static Topology load(const std::string& path);

    // Return the core assigned to `role`, or std::nullopt if no
    // assignment exists for that role (or this topology is empty).
    [[nodiscard]] std::optional<int> core_for(const std::string& role) const;

    // Number of assignments in the loaded topology.
    [[nodiscard]] std::size_t assignment_count() const noexcept { return assignments_.size(); }

    // True if this topology has no assignments (the dev-laptop default).
    [[nodiscard]] bool empty() const noexcept { return assignments_.empty(); }

private:
    // role → core. Kept as a flat hash map because lookups run at
    // thread-creation time, which is typically startup, so the lookup
    // cost never hits the hot path.
    std::unordered_map<std::string, int> assignments_;
};

}  // namespace bpt::common::util

#include "bpt_common/util/topology.h"

#include <toml++/toml.hpp>

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <unistd.h>

namespace bpt::common::util {

namespace {

int online_cpu_count() {
    const long n = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (n <= 0)
        throw std::runtime_error("topology: sysconf(_SC_NPROCESSORS_ONLN) returned " +
                                 std::to_string(n));
    return static_cast<int>(n);
}

}  // namespace

Topology Topology::load(const std::string& path) {
    Topology topo;

    if (path.empty())
        return topo;  // dev-laptop default: no pins

    toml::table root;
    try {
        root = toml::parse_file(path);
    } catch (const toml::parse_error& e) {
        throw std::runtime_error("topology: failed to parse " + path + ": " +
                                 std::string(e.description()));
    }

    const int nproc = online_cpu_count();

    // Optional [host].total_cores sanity check — refuse to start if the
    // machine doesn't match what the topology was authored for, because
    // core indexes beyond nproc would otherwise silently miss.
    if (auto host = root["host"].as_table()) {
        if (auto tc = (*host)["total_cores"].value<int64_t>()) {
            if (*tc != nproc)
                throw std::runtime_error(
                    "topology: host.total_cores=" + std::to_string(*tc) +
                    " does not match online CPU count " + std::to_string(nproc) +
                    " (wrong topology file for this host?)");
        }
    }

    std::unordered_set<int> seen_cores;

    auto assignments = root["assignment"].as_array();
    if (!assignments)
        return topo;  // topology with [host] block but no assignments is legal

    for (const auto& node : *assignments) {
        const auto* entry = node.as_table();
        if (!entry)
            throw std::runtime_error("topology: assignment entry is not a table");

        auto role = (*entry)["role"].value<std::string>();
        auto core = (*entry)["core"].value<int64_t>();
        if (!role)
            throw std::runtime_error("topology: assignment missing required 'role' field");
        if (!core)
            throw std::runtime_error("topology: assignment '" + *role +
                                     "' missing required 'core' field");

        if (*core < 0 || *core >= nproc)
            throw std::runtime_error("topology: role '" + *role + "' core=" +
                                     std::to_string(*core) +
                                     " out of range [0," + std::to_string(nproc) + ")");

        if (!seen_cores.insert(static_cast<int>(*core)).second)
            throw std::runtime_error("topology: core " + std::to_string(*core) +
                                     " assigned twice (conflicting role: '" + *role + "')");

        if (!topo.assignments_.emplace(*role, static_cast<int>(*core)).second)
            throw std::runtime_error("topology: role '" + *role + "' assigned twice");
    }

    return topo;
}

std::optional<int> Topology::core_for(const std::string& role) const {
    auto it = assignments_.find(role);
    if (it == assignments_.end())
        return std::nullopt;
    return it->second;
}

}  // namespace bpt::common::util

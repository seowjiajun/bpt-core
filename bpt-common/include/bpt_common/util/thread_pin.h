#pragma once

// thread_pin.h — CPU thread affinity helpers.
//
// Two APIs:
//   (1) pin_thread_to_cpu(cpu_id, "name")
//       Direct pin to an integer core. Legacy entry point kept for call
//       sites that still pass a raw core number (e.g. per-adapter TOML
//       fallbacks during the topology migration).
//
//   (2) pin_thread_by_role(topology, "role", "name")
//       New entry point. Looks up the role in the central Topology and
//       pins the calling thread to the assigned core. Falls through as
//       a no-op (INFO log) when the role is unassigned — dev-laptop
//       topologies are expected to be empty.
//
// Both log the outcome so operators can grep for "pinned to CPU" in
// startup logs and verify the topology took effect.

#include <pthread.h>
#include <string>
#include <bpt_common/logging.h>
#include <bpt_common/util/topology.h>

namespace bpt::common::util {

// Pin the calling thread to a specific CPU core.
// No-op if cpu_id < 0.  Logs a warning on failure rather than throwing —
// the caller continues running unpinned, just with potential scheduler jitter.
inline void pin_thread_to_cpu(int cpu_id, const char* thread_name) {
    if (cpu_id < 0)
        return;

#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu_id, &cpuset);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    if (rc != 0)
        bpt::common::log::warn("{}: failed to pin to CPU {} (errno={})", thread_name, cpu_id, rc);
    else
        bpt::common::log::info("{}: pinned to CPU {}", thread_name, cpu_id);
#else
    bpt::common::log::warn("{}: CPU pinning not supported on this platform", thread_name);
#endif
}

// Pin the calling thread to the core assigned to `role` in the topology.
// Unassigned role → unpinned (INFO log, no warning). Thread_name is used
// purely for log formatting so the operator can correlate "role=X
// pinned to CPU Y on thread T". Returns true if a pin took effect.
inline bool pin_thread_by_role(const Topology& topology,
                               const std::string& role,
                               const char* thread_name) {
    const auto core = topology.core_for(role);
    if (!core) {
        bpt::common::log::info("{}: role='{}' not in topology — running unpinned",
                               thread_name, role);
        return false;
    }
    pin_thread_to_cpu(*core, thread_name);
    return true;
}

}  // namespace bpt::common::util

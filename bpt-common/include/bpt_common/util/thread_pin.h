#pragma once

// yggdrasil/thread_pin.h — CPU thread affinity helper.
//
// Usage:
//   bpt::common::util::pin_thread_to_cpu(3, "io_thread");   // pin calling thread to CPU 3

#include <pthread.h>
#include <bpt_common/logging.h>

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

}  // namespace bpt::common::util

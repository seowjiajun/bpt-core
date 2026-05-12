#include "bpt_common/util/tsc_clock.h"

#include <bpt_common/logging.h>
#include <time.h>
#include <x86intrin.h>

namespace bpt::common::util {

namespace {

uint64_t read_monotonic_raw_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t read_realtime_ns() noexcept {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

void TscClock::calibrate() noexcept {
    // Warm up the branch predictor and instruction cache for __rdtsc() and
    // clock_gettime().  8 iterations covers two fills of a typical 4-wide
    // reorder buffer; hardcoded — this is a micro-architectural constant, not
    // a tunable parameter.
    for (int i = 0; i < 8; ++i) {
        (void)__rdtsc();
        (void)read_monotonic_raw_ns();
    }

    // Spin-wait for a CLOCK_MONOTONIC_RAW tick boundary to minimise the
    // gap between the kernel timestamp and the TSC read.
    uint64_t mono0 = read_monotonic_raw_ns();
    uint64_t tsc0;
    {
        uint64_t m;
        do {
            m = read_monotonic_raw_ns();
        } while (m == mono0);
        mono0 = m;
        tsc0 = __rdtsc();
    }

    // Sleep ~10ms to accumulate enough TSC ticks for accurate rate measurement.
    struct timespec req = {0, 10'000'000};
    nanosleep(&req, nullptr);

    // Take end sample at a tick boundary.
    uint64_t mono1 = read_monotonic_raw_ns();
    uint64_t tsc1;
    {
        uint64_t m;
        do {
            m = read_monotonic_raw_ns();
        } while (m == mono1);
        mono1 = m;
        tsc1 = __rdtsc();
    }

    ns_per_tsc_ = static_cast<double>(mono1 - mono0) / static_cast<double>(tsc1 - tsc0);

    // Anchor the wall clock to the current TSC.  Repeat 16 times and take
    // the tightest CLOCK_REALTIME pair to minimise jitter.
    uint64_t best_wall = 0, best_tsc = 0, best_gap = UINT64_MAX;
    for (int i = 0; i < 16; ++i) {
        uint64_t w0 = read_realtime_ns();
        uint64_t tsc = __rdtsc();
        uint64_t w1 = read_realtime_ns();
        uint64_t gap = w1 - w0;
        if (gap < best_gap) {
            best_gap = gap;
            best_wall = w0 + gap / 2;
            best_tsc = tsc;
        }
    }

    ref_tsc_ = best_tsc;
    wall_anchor_ns_ = best_wall;

    bpt::common::log::info("TscClock calibrated: {:.4f} GHz, wall anchor err ~{}ns", tsc_ghz(), best_gap / 2);
}

}  // namespace bpt::common::util

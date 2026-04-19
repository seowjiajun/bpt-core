#pragma once

// yggdrasil/latency_histogram.h — Lock-free power-of-2 bucket histogram.
//
// Dependencies: none (C++ stdlib only).
//
// Usage:
//   bpt::common::util::LatencyHistogram hist;
//   hist.record(end_ns - start_ns);
//   auto snap = hist.snapshot_and_reset();
//   bpt::common::log::info("p99={}ns", snap.percentile_ns(0.99));

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <limits>

namespace bpt::common::util {

// Lock-free, power-of-2 bucket latency histogram for HFT hot paths.
//
// Bucket layout: bucket[i] counts values in [2^i, 2^(i+1)) nanoseconds.
//   bucket[0]  → 1 ns             bucket[10] → 1024 ns  (≈1µs)
//   bucket[20] → 1048576 ns (≈1ms) bucket[30] → ≈1s
//
// Resolution: within 2× of the true value (worst case upper-bound estimate).
// record() cost: ~5ns (one atomic fetch_add on hot path).
//
// Thread safety: record() is safe to call concurrently.  snapshot_and_reset()
// is intended for a single reporter thread — individual bucket resets are not
// atomic across all 64 buckets, so at most one sample may be missed at the
// snapshot boundary (acceptable for monitoring).
class LatencyHistogram {
public:
    static constexpr int kBuckets = 64;

    // Record one observation in nanoseconds.  ~5ns cost on modern x86.
    void record(uint64_t ns) noexcept {
        if (ns == 0)
            ns = 1;
        int b = 63 - std::countl_zero(ns);
        counts_[b].fetch_add(1, std::memory_order_relaxed);
    }

    struct Snapshot {
        std::array<uint64_t, 64> counts{};
        uint64_t total{0};
        uint64_t sum_ns{0};

        // Upper-bound estimate of the p-th percentile (0.0–1.0).
        [[nodiscard]] uint64_t percentile_ns(double p) const noexcept {
            if (total == 0)
                return 0;
            auto target = static_cast<uint64_t>(static_cast<double>(total) * p);
            if (target == 0)
                target = 1;
            uint64_t acc = 0;
            for (int i = 0; i < kBuckets; ++i) {
                acc += counts[i];
                if (acc >= target)
                    return (i < kBuckets - 1) ? (2ULL << i) : std::numeric_limits<uint64_t>::max();
            }
            return std::numeric_limits<uint64_t>::max();
        }

        [[nodiscard]] uint64_t max_ns() const noexcept {
            for (int i = kBuckets - 1; i >= 0; --i)
                if (counts[i] > 0)
                    return (i < kBuckets - 1) ? (2ULL << i) : std::numeric_limits<uint64_t>::max();
            return 0;
        }

        [[nodiscard]] uint64_t mean_ns() const noexcept { return total > 0 ? sum_ns / total : 0; }
    };

    // Atomically drain all counters into a Snapshot and reset them to zero.
    [[nodiscard]] Snapshot snapshot_and_reset() noexcept {
        Snapshot s{};
        for (int i = 0; i < kBuckets; ++i) {
            s.counts[i] = counts_[i].exchange(0, std::memory_order_acq_rel);
            s.total += s.counts[i];
            uint64_t mid = (i > 0) ? (3ULL << (i - 1)) : 1ULL;
            s.sum_ns += s.counts[i] * mid;
        }
        return s;
    }

private:
    alignas(64) std::atomic<uint64_t> counts_[kBuckets]{};
};

}  // namespace bpt::common::util

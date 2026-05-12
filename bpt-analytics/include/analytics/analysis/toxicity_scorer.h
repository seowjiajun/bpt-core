#pragma once

#include "analytics/analysis/markout_tracker.h"
#include "analytics/messaging/toxicity_update.h"

#include <cstdint>
#include <deque>

namespace bpt::analytics::analysis {

// Maintains a rolling window of completed markout observations and
// computes per-side toxicity statistics.
//
// Fed completed observations from MarkoutTracker. Produces a
// ToxicityUpdate struct suitable for Aeron publishing.
class ToxicityScorer {
public:
    struct Config {
        std::size_t window_size{50};     // max observations in window
        uint64_t window_duration_ns{0};  // 0 = size-based only
        std::size_t min_samples{5};      // min fills per side before reporting
    };

    ToxicityScorer() : cfg_{} {}
    explicit ToxicityScorer(Config cfg);

    // Add a completed markout observation.
    void add(const MarkoutTracker::Observation& obs);

    // Compute current toxicity stats for an instrument.
    // Returns a ToxicityUpdate with NaN fields when insufficient samples.
    messaging::ToxicityUpdate compute(uint64_t instrument_id, uint64_t now_ns) const;

    std::size_t size() const { return window_.size(); }

private:
    struct SideStats {
        double mean_markout_5s_bps{0.0};
        double adverse_rate{0.0};
        double toxicity_score{0.0};
        uint32_t count{0};
    };

    SideStats compute_side(int side_sign, uint64_t instrument_id) const;

    Config cfg_;
    std::deque<MarkoutTracker::Observation> window_;
};

}  // namespace bpt::analytics::analysis

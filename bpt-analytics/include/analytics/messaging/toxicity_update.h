#pragma once

#include <cstdint>

namespace bpt::analytics::messaging {

// Plain-old-data struct published on Aeron stream 5001.
// Both tyr (publisher) and fenrir (optional subscriber) link this header.
// No SBE — C++-to-C++ within the same codebase; we can add SBE
// encoding later if cross-language consumers appear.
//
// All markout values are in basis points of mid at fill time.
// Positive = favorable (spread capture), negative = adverse (toxic).
struct __attribute__((packed)) ToxicityUpdate {
    uint64_t instrument_id;
    uint64_t timestamp_ns;

    // Rolling mean markout at the 5-second horizon, per side.
    // NaN when insufficient samples.
    double bid_markout_5s_bps;
    double ask_markout_5s_bps;

    // Fraction of fills on each side that had negative 5s markout.
    // 0.0–1.0 range. NaN when insufficient samples.
    double bid_adverse_rate;
    double ask_adverse_rate;

    // Number of fills in the rolling window backing these stats.
    uint32_t bid_sample_count;
    uint32_t ask_sample_count;

    // Composite toxicity score per side — mean markout weighted by
    // adverse rate. More negative = more toxic. Strategy can use
    // ask_toxicity_score < threshold as a suppression signal.
    double bid_toxicity_score;
    double ask_toxicity_score;

    // Fill rate per side — fraction of posted orders that fill vs cancel.
    // NaN when no data. A spike = someone sweeping your quotes.
    double bid_fill_rate;
    double ask_fill_rate;

    // Mean time-to-fill in milliseconds per side.
    // NaN when no fills. A drop = fills coming faster = potential sniper.
    double bid_ttf_ms;
    double ask_ttf_ms;
};

}  // namespace bpt::analytics::messaging

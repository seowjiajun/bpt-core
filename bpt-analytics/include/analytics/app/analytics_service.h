#pragma once

#include "analytics/analysis/fill_rate_tracker.h"
#include "analytics/analysis/markout_tracker.h"
#include "analytics/analysis/toxicity_scorer.h"
#include "analytics/config/settings.h"
#include "analytics/messaging/aeron_bus.h"

#include <bpt_app/app.h>
#include <cstdint>
#include <unordered_map>

namespace bpt::analytics {

class AnalyticsService : public bpt::app::IService {
public:
    AnalyticsService(config::Settings settings, messaging::AnalyticsBus bus);
    void run() override;

private:
    void on_bbo(uint64_t instrument_id, double bid, double ask, uint64_t timestamp_ns);
    void on_exec_fill(uint64_t instrument_id, int side_sign, double fill_price, uint64_t timestamp_ns);
    void on_exec_report(const bpt::messages::ExecutionReport& rpt);
    void maybe_publish(uint64_t now_ns);

    config::Settings settings_;
    messaging::AnalyticsBus bus_;

    // Per-instrument state
    struct InstrumentState {
        double last_mid{0.0};
        analysis::MarkoutTracker tracker;
        analysis::ToxicityScorer scorer;
        analysis::FillRateTracker fill_rate;

        InstrumentState(analysis::MarkoutTracker::Config mt_cfg,
                        analysis::ToxicityScorer::Config ts_cfg,
                        analysis::FillRateTracker::Config fr_cfg)
            : tracker(mt_cfg),
              scorer(ts_cfg),
              fill_rate(fr_cfg) {}
    };

    analysis::MarkoutTracker::Config mt_cfg_;
    analysis::ToxicityScorer::Config ts_cfg_;
    analysis::FillRateTracker::Config fr_cfg_;
    std::unordered_map<uint64_t, InstrumentState> state_;

    uint64_t last_publish_ns_{0};

    InstrumentState& get_or_create(uint64_t instrument_id);
};

}  // namespace bpt::analytics

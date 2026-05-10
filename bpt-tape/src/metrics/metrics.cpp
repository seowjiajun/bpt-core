#include "tape/metrics/metrics.h"

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <string>

namespace bpt::tape::metrics {

TapeMetrics::TapeMetrics(const std::string& host, uint16_t port) {
    registry_ = std::make_shared<prometheus::Registry>();

    exposer_ = std::make_unique<prometheus::Exposer>(host + ":" + std::to_string(port));
    exposer_->RegisterCollectable(registry_);

    // Liveness — single unlabeled gauge, set once.
    auto& h = prometheus::BuildGauge()
                  .Name("bpt_tape_healthy")
                  .Help("1 if bpt-tape is running; 0 on clean shutdown")
                  .Register(*registry_);
    healthy_ = &h.Add({});
    healthy_->Set(1.0);

    last_wslog_write_unix_seconds_fam_ =
        &prometheus::BuildGauge()
             .Name("bpt_tape_last_wslog_write_unix_seconds")
             .Help("Unix-epoch seconds of the last successful wslog write per venue. "
                   "Compare against time() in Alertmanager — staleness > 5min means "
                   "the writer is silently broken (the 2026-05-09 incident).")
             .Register(*registry_);

    frames_written_total_fam_ =
        &prometheus::BuildCounter()
             .Name("bpt_tape_frames_written_total")
             .Help("Total successfully-written wslog records per venue (frames + markers).")
             .Register(*registry_);

    bytes_written_total_fam_ =
        &prometheus::BuildCounter()
             .Name("bpt_tape_bytes_written_total")
             .Help("Total wslog bytes written per venue (record header + payload).")
             .Register(*registry_);

    wslog_rotations_total_fam_ =
        &prometheus::BuildCounter()
             .Name("bpt_tape_wslog_rotations_total")
             .Help("Number of successful wslog file rotations per venue (hourly under default config).")
             .Register(*registry_);

    wslog_rotation_failures_total_fam_ =
        &prometheus::BuildCounter()
             .Name("bpt_tape_wslog_rotation_failures_total")
             .Help("Rotation-open failures per (venue, cause). cause is one of "
                   "'create_directories' or 'fopen' — see RawSpool::ensure_file_open. "
                   "A non-zero value here means the writer aborted (Restart=always recycles).")
             .Register(*registry_);
}

bpt::common::recorder::RawSpool::MetricsHooks
TapeMetrics::hooks_for(const std::string& venue) {
    // Resolve the labeled metric pointers once. References returned by
    // Family::Add() stay valid as long as the family lives — and the
    // family lives as long as TapeMetrics, which outlives every spool by
    // construction (TapeMetrics is built before any spool, destroyed
    // after). Capturing by reference in the lambdas is safe.
    auto& last_write = last_wslog_write_unix_seconds_fam_->Add({{"venue", venue}});
    auto& frames     = frames_written_total_fam_->Add({{"venue", venue}});
    auto& bytes      = bytes_written_total_fam_->Add({{"venue", venue}});
    auto& rotations  = wslog_rotations_total_fam_->Add({{"venue", venue}});

    // rotation_failure is rare and labeled with a cause, so the labeled
    // lookup happens lazily. Capture this + venue by value so the hook
    // outlives the local string.
    auto* failures_fam = wslog_rotation_failures_total_fam_;

    return bpt::common::recorder::RawSpool::MetricsHooks{
        .on_write_success = [&last_write, &frames, &bytes](
                                uint64_t recv_ts_ns, std::size_t total_bytes) {
            last_write.Set(static_cast<double>(recv_ts_ns) / 1e9);
            frames.Increment();
            bytes.Increment(static_cast<double>(total_bytes));
        },
        .on_rotation_success = [&rotations]() {
            rotations.Increment();
        },
        .on_rotation_failure = [failures_fam, venue](std::string_view cause) {
            failures_fam->Add({{"venue", venue}, {"cause", std::string(cause)}})
                        .Increment();
        },
    };
}

void TapeMetrics::shutdown() {
    if (healthy_) healthy_->Set(0.0);
}

}  // namespace bpt::tape::metrics

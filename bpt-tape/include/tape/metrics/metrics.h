#pragma once

/// \file
/// \brief Prometheus metrics exposed by bpt-tape.
///
/// Mirrors the StrategyMetrics / OrderGatewayMetrics shape: registry +
/// exposer + cached metric pointers, no per-call allocation on the hot
/// path. RawSpool integration is via std::function hooks installed
/// through hooks_for(), so bpt-common stays free of any prometheus-cpp
/// dependency — wiring lives in the consumer.
///
/// Metric surface:
///   bpt_tape_healthy                          1 = running, 0 = clean shutdown
///   bpt_tape_last_wslog_write_unix_seconds    freshness gauge per venue
///   bpt_tape_frames_written_total             write counter per venue
///   bpt_tape_bytes_written_total              byte counter per venue
///   bpt_tape_wslog_rotations_total            file rotations per venue
///   bpt_tape_wslog_rotation_failures_total    rotation errors per (venue, cause)
///   bpt_tape_ws_connected                     current WS state per venue
///   bpt_tape_ws_reconnects_total              (re)connect counter per venue
///   bpt_tape_subscriptions                    subscribed-instrument count per venue

#include "bpt_common/recorder/raw_spool.h"

#include <memory>
#include <prometheus/counter.h>
#include <prometheus/exposer.h>
#include <prometheus/family.h>
#include <prometheus/gauge.h>
#include <prometheus/registry.h>
#include <string>

namespace bpt::tape::metrics {

/// \brief Owns the Prometheus registry + HTTP exposer for bpt-tape.
///
/// Constructed once at startup, lives for the process lifetime. Caches
/// raw pointers into prometheus-cpp Family objects so hot-path writes
/// from spool hooks avoid the per-call hash lookup; the registry owns
/// the underlying metric objects.
class TapeMetrics {
public:
    /// \brief Bind the HTTP exposer at host:port.
    ///
    /// In prod, host=0.0.0.0 so the in-VPC monitor host can scrape over
    /// the recorder's private IP; the tape-host security group is the
    /// access control. Use 127.0.0.1 in environments where there's no
    /// in-VPC peer (laptop dev with an SSH tunnel).
    TapeMetrics(const std::string& host, uint16_t port);

    /// \brief Build a RawSpool::MetricsHooks bundle tagged to `venue`.
    ///
    /// Lambdas inside the returned bundle resolve labeled metric
    /// references once (via Family::Add); hot-path callers pay only a
    /// virtual + atomic increment per frame.
    bpt::common::recorder::RawSpool::MetricsHooks hooks_for(const std::string& venue);

    /// \brief Mark `venue`'s WS as connected, incrementing the reconnect
    ///        counter (which includes the bootstrap connect; the +1 is
    ///        one-shot and washes out under rate()).
    void on_ws_connect(const std::string& venue);

    /// \brief Mark `venue`'s WS as disconnected.
    void on_ws_disconnect(const std::string& venue);

    /// \brief Set the current count of subscribed instruments for `venue`.
    ///
    /// Called once after the universe loads. A subsequent drop visible
    /// on the dashboard signals a config regression or mapping shrink.
    void set_subscriptions(const std::string& venue, std::size_t count);

    /// \brief Set healthy=0 so dashboards distinguish a clean stop from
    ///        a scrape-just-vanished crash.
    void shutdown();

private:
    std::shared_ptr<prometheus::Registry> registry_;
    std::unique_ptr<prometheus::Exposer> exposer_;

    prometheus::Gauge* healthy_{};  ///< unlabeled liveness gauge

    // Per-venue Families. Cached as raw pointers — owned by registry_.
    prometheus::Family<prometheus::Gauge>*   last_wslog_write_unix_seconds_fam_{};
    prometheus::Family<prometheus::Counter>* frames_written_total_fam_{};
    prometheus::Family<prometheus::Counter>* bytes_written_total_fam_{};
    prometheus::Family<prometheus::Counter>* wslog_rotations_total_fam_{};
    prometheus::Family<prometheus::Counter>* wslog_rotation_failures_total_fam_{};
    prometheus::Family<prometheus::Gauge>*   ws_connected_fam_{};
    prometheus::Family<prometheus::Counter>* ws_reconnects_total_fam_{};
    prometheus::Family<prometheus::Gauge>*   subscriptions_fam_{};
};

}  // namespace bpt::tape::metrics

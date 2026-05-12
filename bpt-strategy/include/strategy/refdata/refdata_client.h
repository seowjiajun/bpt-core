#pragma once

/// @file
/// AeronRefdataClient — Aeron-backed implementation of IRefdataClient.
/// Subscribes to refdata snapshot, deltas, fees, funding rates, and
/// status streams.
///
/// `RefdataClient` remains as a deprecated alias for AeronRefdataClient so
/// existing call sites compile; new code should depend on IRefdataClient
/// and have AeronRefdataClient injected via the bus factory.

#include "strategy/refdata/i_refdata_client.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/subscriber.h>
#include <memory>
#include <string>

namespace bpt::strategy::refdata {

class AeronRefdataClient : public IRefdataClient {
public:
    AeronRefdataClient(std::shared_ptr<aeron::Aeron> aeron,
                       const std::string& channel,
                       int control_stream,       // 1003 — subscription requests
                       int snapshot_stream,      // 1001 — instrument snapshot
                       int delta_stream,         // 1002 — instrument deltas + heartbeats
                       int fee_schedule_stream,  // 1004 — fee schedule updates
                       int funding_rate_stream,  // 1005 — funding rate updates
                       int status_stream,        // 1006 — ready + error signals
                       uint64_t max_staleness_ns);

    void subscribe(uint64_t correlation_id, std::vector<CanonicalFilter> filters = {}) override;

    int poll(int fragment_limit = 10) override;

    [[nodiscard]] uint64_t last_heartbeat_ns() const override { return last_heartbeat_ns_; }

    [[nodiscard]] const InstrumentCache& cache() const override { return cache_; }
    [[nodiscard]] const FeeCache& fee_cache() const override { return fee_cache_; }
    [[nodiscard]] const FundingRateCache& funding_rate_cache() const override { return funding_rate_cache_; }

private:
    void handle_snapshot_fragment(aeron::AtomicBuffer& buffer,
                                  aeron::util::index_t offset,
                                  aeron::util::index_t length,
                                  aeron::Header& header);

    void handle_delta_fragment(aeron::AtomicBuffer& buffer,
                               aeron::util::index_t offset,
                               aeron::util::index_t length,
                               aeron::Header& header);

    void handle_fee_schedule_fragment(aeron::AtomicBuffer& buf,
                                      aeron::util::index_t offset,
                                      aeron::util::index_t length,
                                      aeron::Header& header);

    void handle_funding_rate_fragment(aeron::AtomicBuffer& buf,
                                      aeron::util::index_t offset,
                                      aeron::util::index_t length,
                                      aeron::Header& header);

    void handle_status_fragment(aeron::AtomicBuffer& buf,
                                aeron::util::index_t offset,
                                aeron::util::index_t length,
                                aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Publisher> ctrl_pub_;
    // Subscriber owns its own FragmentAssembler — essential for the
    // snapshot stream, which can span many frames (thousands of
    // instruments). Without assembly the SBE decoder sees a partial
    // buffer and fails with E108.
    std::unique_ptr<bpt::common::aeron::Subscriber> snap_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> delta_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> fee_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> funding_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> status_sub_;

    FeeCache fee_cache_;
    FundingRateCache funding_rate_cache_;

    InstrumentCache cache_;
    uint64_t correlation_id_{0};
    uint64_t last_heartbeat_ns_{0};
};

/// Backward-compat alias. Existing call sites that use `refdata::RefdataClient`
/// continue to compile because AeronRefdataClient inherits CanonicalFilter +
/// the callback typedefs from IRefdataClient. New code should depend on
/// IRefdataClient (interface).
using RefdataClient = AeronRefdataClient;

}  // namespace bpt::strategy::refdata

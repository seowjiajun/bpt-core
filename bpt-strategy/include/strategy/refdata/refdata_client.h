#pragma once

#include "strategy/refdata/fee_cache.h"
#include "strategy/refdata/funding_rate_cache.h"
#include "strategy/refdata/instrument.h"
#include "strategy/refdata/instrument_cache.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>
#include <messages/RefDataErrorType.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace bpt::strategy::refdata {

class RefdataClient {
public:
    using OnSnapshotFn = std::function<void(const InstrumentCache&)>;
    using OnDeltaFn = std::function<void(const Instrument&, bpt::messages::DeltaUpdateType::Value)>;

    // Canonical filter entry to pre-filter the snapshot server-side.
    // An empty exchange means "any exchange".
    struct CanonicalFilter {
        std::string base;
        std::string quote;
        bpt::messages::InstrumentType::Value instrument_type;
        std::string exchange;
    };

    RefdataClient(std::shared_ptr<aeron::Aeron> aeron,
                  const std::string& channel,
                  int control_stream,       // 1003 — subscription requests
                  int snapshot_stream,      // 1001 — instrument snapshot
                  int delta_stream,         // 1002 — instrument deltas + heartbeats
                  int fee_schedule_stream,  // 1004 — fee schedule updates
                  int funding_rate_stream,  // 1005 — funding rate updates
                  int status_stream,        // 1006 — ready + error signals
                  FeeCache& fee_cache,
                  FundingRateCache& funding_rate_cache);

    // Send a subscription request with canonical filters so the server pre-filters the snapshot.
    // An empty filters vector means subscribe-all (receive the full universe).
    void subscribe(uint64_t correlation_id, std::vector<CanonicalFilter> filters = {});

    // Poll all streams. Returns total fragment count processed.
    int poll(int fragment_limit = 10);

    // Fired once after the snapshot for our correlation ID is fully received.
    OnSnapshotFn on_snapshot_complete;

    // Fired for every delta received after snapshot.
    OnDeltaFn on_delta;

    // Fired when a delta sequence gap is detected. The cache has already been reset;
    // the handler should trigger a resubscription (e.g. call subscribe() again).
    std::function<void()> on_gap_detected;

    // Fired when the refdata service signals it has completed startup for all configured exchanges.
    // exchanges_loaded: bitmask — bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID.
    // Strategy should validate against its configured_exchanges mask and halt if any expected
    // exchange is absent.
    std::function<
        void(uint8_t exchanges_loaded, uint16_t instrument_count, bool fee_schedules_loaded, bool funding_rates_loaded)>
        on_ready;

    // Fired when the refdata service reports a runtime error Strategy must act on.
    std::function<void(bpt::messages::RefDataErrorType::Value error_type,
                       bpt::messages::ExchangeId::Value exchange_id,
                       uint64_t instrument_id)>
        on_error;

    // Nanosecond timestamp of the last heartbeat received (0 if none yet).
    [[nodiscard]] uint64_t last_heartbeat_ns() const { return last_heartbeat_ns_; }

    [[nodiscard]] const InstrumentCache& cache() const { return cache_; }
    [[nodiscard]] const FeeCache& fee_cache() const { return fee_cache_; }
    [[nodiscard]] const FundingRateCache& funding_rate_cache() const { return funding_rate_cache_; }

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

    std::shared_ptr<aeron::Publication> ctrl_pub_;
    std::shared_ptr<aeron::Subscription> snap_sub_;
    std::shared_ptr<aeron::Subscription> delta_sub_;
    std::shared_ptr<aeron::Subscription> fee_sub_;
    std::shared_ptr<aeron::Subscription> funding_sub_;
    std::shared_ptr<aeron::Subscription> status_sub_;

    // FragmentAssemblers reassemble multi-frame Aeron messages before dispatching.
    // The snapshot can be very large (thousands of instruments) and will always span
    // multiple frames; without assembly the SBE decoder receives a partial buffer (E108).
    std::unique_ptr<aeron::FragmentAssembler> snap_assembler_;
    std::unique_ptr<aeron::FragmentAssembler> delta_assembler_;
    std::unique_ptr<aeron::FragmentAssembler> fee_assembler_;
    std::unique_ptr<aeron::FragmentAssembler> funding_assembler_;
    std::unique_ptr<aeron::FragmentAssembler> status_assembler_;

    FeeCache& fee_cache_;
    FundingRateCache& funding_rate_cache_;

    InstrumentCache cache_;
    uint64_t correlation_id_{0};
    uint64_t last_heartbeat_ns_{0};
};

}  // namespace bpt::strategy::refdata

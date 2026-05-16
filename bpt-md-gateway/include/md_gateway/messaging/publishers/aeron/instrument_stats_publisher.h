#pragma once

/// \file
/// \brief Aeron-backed concrete implementation of api::InstrumentStatsPublisher.
///
/// Emits InstrumentStatsUpdate SBE messages (template id 29) on stream 2004.
/// Slow-cadence sibling of FundingRatePublisher — same pattern, same
/// retry-on-backpressure policy.

#include "md_gateway/messaging/codecs/sbe_instrument_stats_codec.h"
#include "md_gateway/messaging/publishers/api/instrument_stats_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <memory>
#include <string>

namespace bpt::md_gateway::messaging::aeron {

/// \brief Aeron implementation of the open-interest outbound port.
///
/// Thread-safety inherited from `bpt::common::aeron::Publisher` —
/// adapter IO threads call publish() concurrently; the underlying
/// aeron::Publication serializes them.
class InstrumentStatsPublisher final : public api::InstrumentStatsPublisher {
public:
    InstrumentStatsPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const InstrumentStatsUpdate& oi) override;

private:
    bpt::common::aeron::Publisher publisher_;
    SbeInstrumentStatsCodec       codec_;
};

}  // namespace bpt::md_gateway::messaging::aeron

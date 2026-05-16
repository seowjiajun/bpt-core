#pragma once

/// \file
/// \brief Aeron-backed concrete implementation of api::FundingRatePublisher.
///
/// Emits FundingRate SBE messages (template id 18) on stream 1005.
/// The wire format and stream ID match what bpt-refdata used to publish
/// before this responsibility moved to the gateway, so strategy-side
/// consumers were not changed during the migration.

#include "md_gateway/messaging/codecs/sbe_funding_rate_codec.h"
#include "md_gateway/messaging/publishers/api/funding_rate_publisher.h"

#include <Aeron.h>

#include <bpt_common/aeron/publisher.h>
#include <memory>
#include <string>

namespace bpt::md_gateway::messaging::aeron {

/// \brief Aeron implementation of the funding-rate outbound port.
///
/// Thread-safety inherited from `bpt::common::aeron::Publisher` —
/// adapter IO threads call publish() concurrently; the underlying
/// aeron::Publication serializes them.
class FundingRatePublisher final : public api::FundingRatePublisher {
public:
    FundingRatePublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const FundingRateUpdate& fr) override;

private:
    bpt::common::aeron::Publisher publisher_;
    SbeFundingRateCodec           codec_;
};

}  // namespace bpt::md_gateway::messaging::aeron

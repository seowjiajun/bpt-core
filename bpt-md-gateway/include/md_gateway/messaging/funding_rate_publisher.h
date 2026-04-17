#pragma once

#include <Aeron.h>

#include <messages/ExchangeId.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace bpt::md_gateway::messaging {

struct FundingRateUpdate {
    uint64_t instrument_id;
    bpt::messages::ExchangeId::Value exchange_id;
    int32_t rate_bps;             // signed; rate * 1e6 (e.g. 0.0001 rate → 100 bps)
    uint64_t next_funding_ts_ns;  // 0 if not provided by exchange
    uint64_t collected_ts_ns;
};

using FundingRateCallback = std::function<void(const FundingRateUpdate&)>;

// Publishes FundingRate SBE messages (template id=18) on stream 1005.
// Same wire format as Refdata previously published — Strategy consumer unchanged.
class FundingRatePublisher {
public:
    FundingRatePublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const FundingRateUpdate& fr);

private:
    std::shared_ptr<aeron::Publication> publication_;
};

}  // namespace bpt::md_gateway::messaging

#pragma once

#include "refdata/messaging/publishers/api/refdata_status_publisher.h"

#include <Aeron.h>

#include <messages/ExchangeId.h>
#include <messages/RefDataErrorType.h>

#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <memory>

namespace bpt::refdata::messaging::aeron {

// Publishes RefDataReady (id=16) and RefDataError (id=17) on stream 1006.
class RefdataStatusPublisher final : public api::RefdataStatusPublisher {
public:
    RefdataStatusPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    // Published once after all enabled exchange adapters have completed
    // their initial snapshot fetch.
    void publish_ready(uint8_t exchanges_loaded,  // bitmask bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID
                       uint16_t instrument_count,
                       bool fee_schedules_loaded) override;

    // Published whenever a runtime error occurs that Strategy must act on.
    void publish_error(bpt::messages::RefDataErrorType::Value error_type,
                       bpt::messages::ExchangeId::Value exchange_id,
                       uint64_t instrument_id = 0) override;

private:
    std::shared_ptr<::aeron::Publication> publication_;
};

}  // namespace bpt::refdata::messaging::aeron

#pragma once

/// \file
/// \brief Outbound port: refdata service status (ready / error) publish.

#include <messages/ExchangeId.h>
#include <messages/RefDataErrorType.h>

#include <cstdint>

namespace bpt::refdata::port {

class IRefdataStatusSink {
public:
    virtual ~IRefdataStatusSink() = default;

    /// \brief Published once after all enabled exchange adapters have completed
    ///        their initial snapshot fetch.
    /// \param exchanges_loaded bitmask: bit0=BINANCE, bit1=OKX, bit2=HYPERLIQUID.
    virtual void publish_ready(uint8_t exchanges_loaded,
                               uint16_t instrument_count,
                               bool fee_schedules_loaded) = 0;

    /// \brief Published whenever a runtime error occurs that Strategy must act on.
    virtual void publish_error(bpt::messages::RefDataErrorType::Value error_type,
                               bpt::messages::ExchangeId::Value exchange_id,
                               uint64_t instrument_id = 0) = 0;
};

}  // namespace bpt::refdata::port

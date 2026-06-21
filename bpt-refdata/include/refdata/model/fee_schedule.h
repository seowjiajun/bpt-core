#pragma once

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>

#include <cstdint>

namespace bpt::refdata::model {

struct FeeScheduleState {
    bpt::messages::ExchangeId::Value exchange_id;
    uint64_t instrument_id;  // 0 = applies to all instruments on this exchange
    bpt::messages::InstrumentType::Value instrument_type;
    int16_t maker_fee_bps;  // signed; negative = rebate
    int16_t taker_fee_bps;
    uint64_t updated_ts;  // UTC nanosecond epoch when fetched from exchange
};

}  // namespace bpt::refdata::model

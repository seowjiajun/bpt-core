#pragma once

#include <cstdint>
#include <string>

namespace bpt::order_gateway::refdata {

enum class InstrumentType : uint8_t { SPOT = 0, FUTURE = 1, PERPETUAL = 2, OPTION = 3 };

enum class InstrumentStatus : uint8_t { ACTIVE = 0, INACTIVE = 1, HALTED = 2 };

enum class OptionSide : uint8_t { NA = 0, CALL = 1, PUT = 2 };

struct Instrument {
    uint64_t instrument_id{0};
    std::string symbol;
    std::string exchange;
    std::string base_currency;
    std::string quote_currency;
    InstrumentType type{InstrumentType::SPOT};
    InstrumentStatus status{InstrumentStatus::ACTIVE};
    double lot_size{0.0};
    double tick_size{0.0};
    double contract_size{1.0};
    uint32_t expiry_date{0};  // YYYYMMDD, 0 if not applicable
    OptionSide option_side{OptionSide::NA};
    double strike_price{0.0};
};

}  // namespace bpt::order_gateway::refdata

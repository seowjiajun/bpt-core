#pragma once

#include "refdata/model/types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace bpt::refdata::model {

struct Instrument {
    uint64_t inst_uid;

    std::string venue;
    std::string venue_symbol;
    std::string display_name;

    InstrumentType inst_type;
    std::string base;
    std::string quote;

    double tick_size;
    double lot_size;
    double contract_multiplier;

    InstrumentStatus status;
    uint64_t version;

    // Optional fields for future use or specific types
    std::optional<uint64_t> expiry_timestamp;
    std::optional<double> strike_price;

    bool operator==(const Instrument& other) const = default;
};

}  // namespace bpt::refdata::model

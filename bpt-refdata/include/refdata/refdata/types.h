#pragma once

#include <cstdint>
#include <string_view>

namespace bpt::refdata::refdata {

enum class InstrumentType : uint8_t { SPOT, PERP, FUTURE, OPTION, UNKNOWN };

enum class InstrumentStatus : uint8_t { ACTIVE, DELISTED, HALTED, UNKNOWN };

constexpr std::string_view toString(InstrumentType type) {
    switch (type) {
        case InstrumentType::SPOT:
            return "SPOT";
        case InstrumentType::PERP:
            return "PERP";
        case InstrumentType::FUTURE:
            return "FUTURE";
        case InstrumentType::OPTION:
            return "OPTION";
        default:
            return "UNKNOWN";
    }
}

constexpr std::string_view toString(InstrumentStatus status) {
    switch (status) {
        case InstrumentStatus::ACTIVE:
            return "ACTIVE";
        case InstrumentStatus::DELISTED:
            return "DELISTED";
        case InstrumentStatus::HALTED:
            return "HALTED";
        default:
            return "UNKNOWN";
    }
}

}  // namespace bpt::refdata::refdata

namespace bpt::refdata::refdata {
constexpr InstrumentType instrumentTypeFromString(std::string_view str) {
    if (str == "SPOT")
        return InstrumentType::SPOT;
    if (str == "PERP")
        return InstrumentType::PERP;
    if (str == "FUTURE")
        return InstrumentType::FUTURE;
    if (str == "OPTION")
        return InstrumentType::OPTION;
    return InstrumentType::UNKNOWN;
}
}  // namespace bpt::refdata::refdata

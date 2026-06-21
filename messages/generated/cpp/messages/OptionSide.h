/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BPT_MESSAGES_OPTIONSIDE_CXX_H_
#define _BPT_MESSAGES_OPTIONSIDE_CXX_H_

#if !defined(__STDC_LIMIT_MACROS)
#define __STDC_LIMIT_MACROS 1
#endif

#include <cstdint>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

#define SBE_NULLVALUE_INT8 (std::numeric_limits<std::int8_t>::min)()
#define SBE_NULLVALUE_INT16 (std::numeric_limits<std::int16_t>::min)()
#define SBE_NULLVALUE_INT32 (std::numeric_limits<std::int32_t>::min)()
#define SBE_NULLVALUE_INT64 (std::numeric_limits<std::int64_t>::min)()
#define SBE_NULLVALUE_UINT8 (std::numeric_limits<std::uint8_t>::max)()
#define SBE_NULLVALUE_UINT16 (std::numeric_limits<std::uint16_t>::max)()
#define SBE_NULLVALUE_UINT32 (std::numeric_limits<std::uint32_t>::max)()
#define SBE_NULLVALUE_UINT64 (std::numeric_limits<std::uint64_t>::max)()

namespace bpt {
namespace messages {

class OptionSide {
public:
    enum Value {
        CALL = static_cast<std::uint8_t>(0),
        PUT = static_cast<std::uint8_t>(1),
        NA = static_cast<std::uint8_t>(2),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static OptionSide::Value get(const std::uint8_t value) {
        switch (value) {
            case static_cast<std::uint8_t>(0):
                return CALL;
            case static_cast<std::uint8_t>(1):
                return PUT;
            case static_cast<std::uint8_t>(2):
                return NA;
            case static_cast<std::uint8_t>(255):
                return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum OptionSide [E103]");
    }

    static const char* c_str(const OptionSide::Value value) {
        switch (value) {
            case CALL:
                return "CALL";
            case PUT:
                return "PUT";
            case NA:
                return "NA";
            case NULL_VALUE:
                return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum OptionSide [E103]:");
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, OptionSide::Value m) {
        return os << OptionSide::c_str(m);
    }
};

}  // namespace messages
}  // namespace bpt

#endif

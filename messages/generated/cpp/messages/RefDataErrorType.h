/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_SINDRIERRORTYPE_CXX_H_
#define _BIFROST_PROTOCOL_SINDRIERRORTYPE_CXX_H_

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

class RefDataErrorType {
public:
    enum Value {
        EXCHANGE_NOT_CONFIGURED = static_cast<std::uint8_t>(1),
        SNAPSHOT_FAILED = static_cast<std::uint8_t>(2),
        DELTA_FEED_LOST = static_cast<std::uint8_t>(3),
        LOOKUP_FAILED = static_cast<std::uint8_t>(4),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static RefDataErrorType::Value get(const std::uint8_t value) {
        switch (value) {
            case static_cast<std::uint8_t>(1):
                return EXCHANGE_NOT_CONFIGURED;
            case static_cast<std::uint8_t>(2):
                return SNAPSHOT_FAILED;
            case static_cast<std::uint8_t>(3):
                return DELTA_FEED_LOST;
            case static_cast<std::uint8_t>(4):
                return LOOKUP_FAILED;
            case static_cast<std::uint8_t>(255):
                return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum RefDataErrorType [E103]");
    }

    static const char* c_str(const RefDataErrorType::Value value) {
        switch (value) {
            case EXCHANGE_NOT_CONFIGURED:
                return "EXCHANGE_NOT_CONFIGURED";
            case SNAPSHOT_FAILED:
                return "SNAPSHOT_FAILED";
            case DELTA_FEED_LOST:
                return "DELTA_FEED_LOST";
            case LOOKUP_FAILED:
                return "LOOKUP_FAILED";
            case NULL_VALUE:
                return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum RefDataErrorType [E103]:");
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os,
                                                         RefDataErrorType::Value m) {
        return os << RefDataErrorType::c_str(m);
    }
};

}  // namespace protocol
}  // namespace bpt

#endif

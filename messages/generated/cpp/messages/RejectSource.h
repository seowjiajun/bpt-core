/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_REJECTSOURCE_CXX_H_
#define _BIFROST_PROTOCOL_REJECTSOURCE_CXX_H_

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

class RejectSource {
public:
    enum Value {
        EXCHANGE = static_cast<std::uint8_t>(0),
        GATEWAY = static_cast<std::uint8_t>(1),
        RISK = static_cast<std::uint8_t>(2),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static RejectSource::Value get(const std::uint8_t value) {
        switch (value) {
            case static_cast<std::uint8_t>(0):
                return EXCHANGE;
            case static_cast<std::uint8_t>(1):
                return GATEWAY;
            case static_cast<std::uint8_t>(2):
                return RISK;
            case static_cast<std::uint8_t>(255):
                return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum RejectSource [E103]");
    }

    static const char* c_str(const RejectSource::Value value) {
        switch (value) {
            case EXCHANGE:
                return "EXCHANGE";
            case GATEWAY:
                return "GATEWAY";
            case RISK:
                return "RISK";
            case NULL_VALUE:
                return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum RejectSource [E103]:");
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, RejectSource::Value m) {
        return os << RejectSource::c_str(m);
    }
};

}  // namespace protocol
}  // namespace bpt

#endif

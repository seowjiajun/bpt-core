/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_EXECSTATUS_CXX_H_
#define _BIFROST_PROTOCOL_EXECSTATUS_CXX_H_

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

class ExecStatus {
public:
    enum Value {
        ACKED = static_cast<std::uint8_t>(0),
        FILLED = static_cast<std::uint8_t>(1),
        PARTIAL = static_cast<std::uint8_t>(2),
        REJECTED = static_cast<std::uint8_t>(3),
        CANCELLED = static_cast<std::uint8_t>(4),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static ExecStatus::Value get(const std::uint8_t value) {
        switch (value) {
            case static_cast<std::uint8_t>(0):
                return ACKED;
            case static_cast<std::uint8_t>(1):
                return FILLED;
            case static_cast<std::uint8_t>(2):
                return PARTIAL;
            case static_cast<std::uint8_t>(3):
                return REJECTED;
            case static_cast<std::uint8_t>(4):
                return CANCELLED;
            case static_cast<std::uint8_t>(255):
                return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum ExecStatus [E103]");
    }

    static const char* c_str(const ExecStatus::Value value) {
        switch (value) {
            case ACKED:
                return "ACKED";
            case FILLED:
                return "FILLED";
            case PARTIAL:
                return "PARTIAL";
            case REJECTED:
                return "REJECTED";
            case CANCELLED:
                return "CANCELLED";
            case NULL_VALUE:
                return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum ExecStatus [E103]:");
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, ExecStatus::Value m) {
        return os << ExecStatus::c_str(m);
    }
};

}  // namespace protocol
}  // namespace bpt

#endif

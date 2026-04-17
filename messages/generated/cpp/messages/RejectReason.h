/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_REJECTREASON_CXX_H_
#define _BIFROST_PROTOCOL_REJECTREASON_CXX_H_

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

class RejectReason {
public:
    enum Value {
        OK = static_cast<std::uint8_t>(0),
        INVALID_PRICE = static_cast<std::uint8_t>(1),
        INVALID_QTY = static_cast<std::uint8_t>(2),
        INSUFFICIENT_BALANCE = static_cast<std::uint8_t>(3),
        RATE_LIMITED = static_cast<std::uint8_t>(4),
        EXCHANGE_ERROR = static_cast<std::uint8_t>(5),
        RISK_REJECTED = static_cast<std::uint8_t>(6),
        DUPLICATE_ORDER_ID = static_cast<std::uint8_t>(7),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static RejectReason::Value get(const std::uint8_t value) {
        switch (value) {
            case static_cast<std::uint8_t>(0):
                return OK;
            case static_cast<std::uint8_t>(1):
                return INVALID_PRICE;
            case static_cast<std::uint8_t>(2):
                return INVALID_QTY;
            case static_cast<std::uint8_t>(3):
                return INSUFFICIENT_BALANCE;
            case static_cast<std::uint8_t>(4):
                return RATE_LIMITED;
            case static_cast<std::uint8_t>(5):
                return EXCHANGE_ERROR;
            case static_cast<std::uint8_t>(6):
                return RISK_REJECTED;
            case static_cast<std::uint8_t>(7):
                return DUPLICATE_ORDER_ID;
            case static_cast<std::uint8_t>(255):
                return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum RejectReason [E103]");
    }

    static const char* c_str(const RejectReason::Value value) {
        switch (value) {
            case OK:
                return "OK";
            case INVALID_PRICE:
                return "INVALID_PRICE";
            case INVALID_QTY:
                return "INVALID_QTY";
            case INSUFFICIENT_BALANCE:
                return "INSUFFICIENT_BALANCE";
            case RATE_LIMITED:
                return "RATE_LIMITED";
            case EXCHANGE_ERROR:
                return "EXCHANGE_ERROR";
            case RISK_REJECTED:
                return "RISK_REJECTED";
            case DUPLICATE_ORDER_ID:
                return "DUPLICATE_ORDER_ID";
            case NULL_VALUE:
                return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum RejectReason [E103]:");
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, RejectReason::Value m) {
        return os << RejectReason::c_str(m);
    }
};

}  // namespace protocol
}  // namespace bpt

#endif

/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_FEECURRENCY_CXX_H_
#define _BIFROST_PROTOCOL_FEECURRENCY_CXX_H_

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

class FeeCurrency {
public:
    enum Value {
        USDT = static_cast<std::uint8_t>(0),
        BTC = static_cast<std::uint8_t>(1),
        ETH = static_cast<std::uint8_t>(2),
        BNB = static_cast<std::uint8_t>(3),
        USD = static_cast<std::uint8_t>(4),
        NULL_VALUE = static_cast<std::uint8_t>(255)
    };

    static FeeCurrency::Value get(const std::uint8_t value) {
        switch (value) {
            case static_cast<std::uint8_t>(0):
                return USDT;
            case static_cast<std::uint8_t>(1):
                return BTC;
            case static_cast<std::uint8_t>(2):
                return ETH;
            case static_cast<std::uint8_t>(3):
                return BNB;
            case static_cast<std::uint8_t>(4):
                return USD;
            case static_cast<std::uint8_t>(255):
                return NULL_VALUE;
        }

        throw std::runtime_error("unknown value for enum FeeCurrency [E103]");
    }

    static const char* c_str(const FeeCurrency::Value value) {
        switch (value) {
            case USDT:
                return "USDT";
            case BTC:
                return "BTC";
            case ETH:
                return "ETH";
            case BNB:
                return "BNB";
            case USD:
                return "USD";
            case NULL_VALUE:
                return "NULL_VALUE";
        }

        throw std::runtime_error("unknown value for enum FeeCurrency [E103]:");
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& os, FeeCurrency::Value m) {
        return os << FeeCurrency::c_str(m);
    }
};

}  // namespace protocol
}  // namespace bpt

#endif

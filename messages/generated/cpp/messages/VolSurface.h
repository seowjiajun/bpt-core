/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BPT_MESSAGES_VOLSURFACE_CXX_H_
#define _BPT_MESSAGES_VOLSURFACE_CXX_H_

#if __cplusplus >= 201103L
#  define SBE_CONSTEXPR constexpr
#  define SBE_NOEXCEPT noexcept
#else
#  define SBE_CONSTEXPR
#  define SBE_NOEXCEPT
#endif

#if __cplusplus >= 201703L
#  include <string_view>
#  define SBE_NODISCARD [[nodiscard]]
#else
#  define SBE_NODISCARD
#endif

#if !defined(__STDC_LIMIT_MACROS)
#  define __STDC_LIMIT_MACROS 1
#endif

#include <cstdint>
#include <limits>
#include <cstring>
#include <iomanip>
#include <ostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>
#include <tuple>

#if defined(WIN32) || defined(_WIN32)
#  define SBE_BIG_ENDIAN_ENCODE_16(v) _byteswap_ushort(v)
#  define SBE_BIG_ENDIAN_ENCODE_32(v) _byteswap_ulong(v)
#  define SBE_BIG_ENDIAN_ENCODE_64(v) _byteswap_uint64(v)
#  define SBE_LITTLE_ENDIAN_ENCODE_16(v) (v)
#  define SBE_LITTLE_ENDIAN_ENCODE_32(v) (v)
#  define SBE_LITTLE_ENDIAN_ENCODE_64(v) (v)
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#  define SBE_BIG_ENDIAN_ENCODE_16(v) __builtin_bswap16(v)
#  define SBE_BIG_ENDIAN_ENCODE_32(v) __builtin_bswap32(v)
#  define SBE_BIG_ENDIAN_ENCODE_64(v) __builtin_bswap64(v)
#  define SBE_LITTLE_ENDIAN_ENCODE_16(v) (v)
#  define SBE_LITTLE_ENDIAN_ENCODE_32(v) (v)
#  define SBE_LITTLE_ENDIAN_ENCODE_64(v) (v)
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define SBE_LITTLE_ENDIAN_ENCODE_16(v) __builtin_bswap16(v)
#  define SBE_LITTLE_ENDIAN_ENCODE_32(v) __builtin_bswap32(v)
#  define SBE_LITTLE_ENDIAN_ENCODE_64(v) __builtin_bswap64(v)
#  define SBE_BIG_ENDIAN_ENCODE_16(v) (v)
#  define SBE_BIG_ENDIAN_ENCODE_32(v) (v)
#  define SBE_BIG_ENDIAN_ENCODE_64(v) (v)
#else
#  error "Byte Ordering of platform not determined. Set __BYTE_ORDER__ manually before including this file."
#endif

#if !defined(SBE_BOUNDS_CHECK_EXPECT)
#  if defined(SBE_NO_BOUNDS_CHECK)
#    define SBE_BOUNDS_CHECK_EXPECT(exp, c) (false)
#  elif defined(_MSC_VER)
#    define SBE_BOUNDS_CHECK_EXPECT(exp, c) (exp)
#  else 
#    define SBE_BOUNDS_CHECK_EXPECT(exp, c) (__builtin_expect(exp, c))
#  endif

#endif

#define SBE_FLOAT_NAN std::numeric_limits<float>::quiet_NaN()
#define SBE_DOUBLE_NAN std::numeric_limits<double>::quiet_NaN()
#define SBE_NULLVALUE_INT8 (std::numeric_limits<std::int8_t>::min)()
#define SBE_NULLVALUE_INT16 (std::numeric_limits<std::int16_t>::min)()
#define SBE_NULLVALUE_INT32 (std::numeric_limits<std::int32_t>::min)()
#define SBE_NULLVALUE_INT64 (std::numeric_limits<std::int64_t>::min)()
#define SBE_NULLVALUE_UINT8 (std::numeric_limits<std::uint8_t>::max)()
#define SBE_NULLVALUE_UINT16 (std::numeric_limits<std::uint16_t>::max)()
#define SBE_NULLVALUE_UINT32 (std::numeric_limits<std::uint32_t>::max)()
#define SBE_NULLVALUE_UINT64 (std::numeric_limits<std::uint64_t>::max)()


#include "MessageHeader.h"
#include "InstrumentType.h"
#include "TradeSide.h"
#include "GroupSizeEncoding.h"
#include "ExecStatus.h"
#include "OrderType.h"
#include "InstrumentStatus.h"
#include "RefDataErrorType.h"
#include "TimeInForce.h"
#include "BacktestCommand.h"
#include "RejectReason.h"
#include "AckStatus.h"
#include "OptionSide.h"
#include "RejectSource.h"
#include "ExchangeId.h"
#include "DeltaUpdateType.h"
#include "OrderSide.h"
#include "FeeCurrency.h"

namespace bpt {
namespace messages {

class VolSurface
{
private:
    char *m_buffer = nullptr;
    std::uint64_t m_bufferLength = 0;
    std::uint64_t m_offset = 0;
    std::uint64_t m_position = 0;
    std::uint64_t m_actingBlockLength = 0;
    std::uint64_t m_actingVersion = 0;

    inline std::uint64_t *sbePositionPtr() SBE_NOEXCEPT
    {
        return &m_position;
    }

public:
    static const std::uint16_t SBE_BLOCK_LENGTH = static_cast<std::uint16_t>(41);
    static const std::uint16_t SBE_TEMPLATE_ID = static_cast<std::uint16_t>(21);
    static const std::uint16_t SBE_SCHEMA_ID = static_cast<std::uint16_t>(1);
    static const std::uint16_t SBE_SCHEMA_VERSION = static_cast<std::uint16_t>(13);
    static constexpr const char* SBE_SEMANTIC_VERSION = "1.13.0";

    enum MetaAttribute
    {
        EPOCH, TIME_UNIT, SEMANTIC_TYPE, PRESENCE
    };

    union sbe_float_as_uint_u
    {
        float fp_value;
        std::uint32_t uint_value;
    };

    union sbe_double_as_uint_u
    {
        double fp_value;
        std::uint64_t uint_value;
    };

    using messageHeader = MessageHeader;

    VolSurface() = default;

    VolSurface(
        char *buffer,
        const std::uint64_t offset,
        const std::uint64_t bufferLength,
        const std::uint64_t actingBlockLength,
        const std::uint64_t actingVersion) :
        m_buffer(buffer),
        m_bufferLength(bufferLength),
        m_offset(offset),
        m_position(sbeCheckPosition(offset + actingBlockLength)),
        m_actingBlockLength(actingBlockLength),
        m_actingVersion(actingVersion)
    {
    }

    VolSurface(char *buffer, const std::uint64_t bufferLength) :
        VolSurface(buffer, 0, bufferLength, sbeBlockLength(), sbeSchemaVersion())
    {
    }

    VolSurface(
        char *buffer,
        const std::uint64_t bufferLength,
        const std::uint64_t actingBlockLength,
        const std::uint64_t actingVersion) :
        VolSurface(buffer, 0, bufferLength, actingBlockLength, actingVersion)
    {
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeBlockLength() SBE_NOEXCEPT
    {
        return static_cast<std::uint16_t>(41);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sbeBlockAndHeaderLength() SBE_NOEXCEPT
    {
        return messageHeader::encodedLength() + sbeBlockLength();
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeTemplateId() SBE_NOEXCEPT
    {
        return static_cast<std::uint16_t>(21);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeSchemaId() SBE_NOEXCEPT
    {
        return static_cast<std::uint16_t>(1);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeSchemaVersion() SBE_NOEXCEPT
    {
        return static_cast<std::uint16_t>(13);
    }

    SBE_NODISCARD static const char *sbeSemanticVersion() SBE_NOEXCEPT
    {
        return "1.13.0";
    }

    SBE_NODISCARD static SBE_CONSTEXPR const char *sbeSemanticType() SBE_NOEXCEPT
    {
        return "";
    }

    SBE_NODISCARD std::uint64_t offset() const SBE_NOEXCEPT
    {
        return m_offset;
    }

    VolSurface &wrapForEncode(char *buffer, const std::uint64_t offset, const std::uint64_t bufferLength)
    {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    VolSurface &wrapAndApplyHeader(char *buffer, const std::uint64_t offset, const std::uint64_t bufferLength)
    {
        messageHeader hdr(buffer, offset, bufferLength, sbeSchemaVersion());

        hdr
            .blockLength(sbeBlockLength())
            .templateId(sbeTemplateId())
            .schemaId(sbeSchemaId())
            .version(sbeSchemaVersion());

        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset + messageHeader::encodedLength();
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    VolSurface &wrapForDecode(
        char *buffer,
        const std::uint64_t offset,
        const std::uint64_t actingBlockLength,
        const std::uint64_t actingVersion,
        const std::uint64_t bufferLength)
    {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = actingBlockLength;
        m_actingVersion = actingVersion;
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    VolSurface &sbeRewind()
    {
        return wrapForDecode(m_buffer, m_offset, m_actingBlockLength, m_actingVersion, m_bufferLength);
    }

    SBE_NODISCARD std::uint64_t sbePosition() const SBE_NOEXCEPT
    {
        return m_position;
    }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    std::uint64_t sbeCheckPosition(const std::uint64_t position)
    {
        if (SBE_BOUNDS_CHECK_EXPECT((position > m_bufferLength), false))
        {
            throw std::runtime_error("buffer too short [E100]");
        }
        return position;
    }

    void sbePosition(const std::uint64_t position)
    {
        m_position = sbeCheckPosition(position);
    }

    SBE_NODISCARD std::uint64_t encodedLength() const SBE_NOEXCEPT
    {
        return sbePosition() - m_offset;
    }

    SBE_NODISCARD std::uint64_t decodeLength() const
    {
        VolSurface skipper(m_buffer, m_offset, m_bufferLength, sbeBlockLength(), m_actingVersion);
        skipper.skip();
        return skipper.encodedLength();
    }

    SBE_NODISCARD const char *buffer() const SBE_NOEXCEPT
    {
        return m_buffer;
    }

    SBE_NODISCARD char *buffer() SBE_NOEXCEPT
    {
        return m_buffer;
    }

    SBE_NODISCARD std::uint64_t bufferLength() const SBE_NOEXCEPT
    {
        return m_bufferLength;
    }

    SBE_NODISCARD std::uint64_t actingVersion() const SBE_NOEXCEPT
    {
        return m_actingVersion;
    }

    SBE_NODISCARD static const char *timestampNsMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
    {
        switch (metaAttribute)
        {
            case MetaAttribute::PRESENCE: return "required";
            default: return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t timestampNsId() SBE_NOEXCEPT
    {
        return 1;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t timestampNsSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool timestampNsInActingVersion() SBE_NOEXCEPT
    {
        return true;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t timestampNsEncodingOffset() SBE_NOEXCEPT
    {
        return 0;
    }

    static SBE_CONSTEXPR std::uint64_t timestampNsNullValue() SBE_NOEXCEPT
    {
        return SBE_NULLVALUE_UINT64;
    }

    static SBE_CONSTEXPR std::uint64_t timestampNsMinValue() SBE_NOEXCEPT
    {
        return UINT64_C(0x0);
    }

    static SBE_CONSTEXPR std::uint64_t timestampNsMaxValue() SBE_NOEXCEPT
    {
        return UINT64_C(0xfffffffffffffffe);
    }

    static SBE_CONSTEXPR std::size_t timestampNsEncodingLength() SBE_NOEXCEPT
    {
        return 8;
    }

    SBE_NODISCARD std::uint64_t timestampNs() const SBE_NOEXCEPT
    {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    VolSurface &timestampNs(const std::uint64_t value) SBE_NOEXCEPT
    {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char *exchangeIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
    {
        switch (metaAttribute)
        {
            case MetaAttribute::PRESENCE: return "required";
            default: return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t exchangeIdId() SBE_NOEXCEPT
    {
        return 2;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeIdSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool exchangeIdInActingVersion() SBE_NOEXCEPT
    {
        return true;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeIdEncodingOffset() SBE_NOEXCEPT
    {
        return 8;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeIdEncodingLength() SBE_NOEXCEPT
    {
        return 1;
    }

    SBE_NODISCARD std::uint8_t exchangeIdRaw() const SBE_NOEXCEPT
    {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint8_t));
        return (val);
    }

    SBE_NODISCARD ExchangeId::Value exchangeId() const
    {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint8_t));
        return ExchangeId::get((val));
    }

    VolSurface &exchangeId(const ExchangeId::Value value) SBE_NOEXCEPT
    {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 8, &val, sizeof(std::uint8_t));
        return *this;
    }

    SBE_NODISCARD static const char *underlyingMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
    {
        switch (metaAttribute)
        {
            case MetaAttribute::PRESENCE: return "required";
            default: return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t underlyingId() SBE_NOEXCEPT
    {
        return 3;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t underlyingSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool underlyingInActingVersion() SBE_NOEXCEPT
    {
        return true;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t underlyingEncodingOffset() SBE_NOEXCEPT
    {
        return 9;
    }

    static SBE_CONSTEXPR char underlyingNullValue() SBE_NOEXCEPT
    {
        return static_cast<char>(0);
    }

    static SBE_CONSTEXPR char underlyingMinValue() SBE_NOEXCEPT
    {
        return static_cast<char>(32);
    }

    static SBE_CONSTEXPR char underlyingMaxValue() SBE_NOEXCEPT
    {
        return static_cast<char>(126);
    }

    static SBE_CONSTEXPR std::size_t underlyingEncodingLength() SBE_NOEXCEPT
    {
        return 24;
    }

    static SBE_CONSTEXPR std::uint64_t underlyingLength() SBE_NOEXCEPT
    {
        return 24;
    }

    SBE_NODISCARD const char *underlying() const SBE_NOEXCEPT
    {
        return m_buffer + m_offset + 9;
    }

    SBE_NODISCARD char *underlying() SBE_NOEXCEPT
    {
        return m_buffer + m_offset + 9;
    }

    SBE_NODISCARD char underlying(const std::uint64_t index) const
    {
        if (index >= 24)
        {
            throw std::runtime_error("index out of range for underlying [E104]");
        }

        char val;
        std::memcpy(&val, m_buffer + m_offset + 9 + (index * 1), sizeof(char));
        return (val);
    }

    VolSurface &underlying(const std::uint64_t index, const char value)
    {
        if (index >= 24)
        {
            throw std::runtime_error("index out of range for underlying [E105]");
        }

        char val = (value);
        std::memcpy(m_buffer + m_offset + 9 + (index * 1), &val, sizeof(char));
        return *this;
    }

    std::uint64_t getUnderlying(char *const dst, const std::uint64_t length) const
    {
        if (length > 24)
        {
            throw std::runtime_error("length too large for getUnderlying [E106]");
        }

        std::memcpy(dst, m_buffer + m_offset + 9, sizeof(char) * static_cast<std::size_t>(length));
        return length;
    }

    VolSurface &putUnderlying(const char *const src) SBE_NOEXCEPT
    {
        std::memcpy(m_buffer + m_offset + 9, src, sizeof(char) * 24);
        return *this;
    }

    SBE_NODISCARD std::string getUnderlyingAsString() const
    {
        const char *buffer = m_buffer + m_offset + 9;
        std::size_t length = 0;

        for (; length < 24 && *(buffer + length) != '\0'; ++length);
        std::string result(buffer, length);

        return result;
    }

    std::string getUnderlyingAsJsonEscapedString()
    {
        std::ostringstream oss;
        std::string s = getUnderlyingAsString();

        for (const auto c : s)
        {
            switch (c)
            {
                case '"': oss << "\\\""; break;
                case '\\': oss << "\\\\"; break;
                case '\b': oss << "\\b"; break;
                case '\f': oss << "\\f"; break;
                case '\n': oss << "\\n"; break;
                case '\r': oss << "\\r"; break;
                case '\t': oss << "\\t"; break;

                default:
                    if ('\x00' <= c && c <= '\x1f')
                    {
                        oss << "\\u" << std::hex << std::setw(4)
                            << std::setfill('0') << (int)(c);
                    }
                    else
                    {
                        oss << c;
                    }
            }
        }

        return oss.str();
    }

    #if __cplusplus >= 201703L
    SBE_NODISCARD std::string_view getUnderlyingAsStringView() const SBE_NOEXCEPT
    {
        const char *buffer = m_buffer + m_offset + 9;
        std::size_t length = 0;

        for (; length < 24 && *(buffer + length) != '\0'; ++length);
        std::string_view result(buffer, length);

        return result;
    }
    #endif

    #if __cplusplus >= 201703L
    VolSurface &putUnderlying(const std::string_view str)
    {
        const std::size_t srcLength = str.length();
        if (srcLength > 24)
        {
            throw std::runtime_error("string too large for putUnderlying [E106]");
        }

        std::memcpy(m_buffer + m_offset + 9, str.data(), srcLength);
        for (std::size_t start = srcLength; start < 24; ++start)
        {
            m_buffer[m_offset + 9 + start] = 0;
        }

        return *this;
    }
    #else
    VolSurface &putUnderlying(const std::string &str)
    {
        const std::size_t srcLength = str.length();
        if (srcLength > 24)
        {
            throw std::runtime_error("string too large for putUnderlying [E106]");
        }

        std::memcpy(m_buffer + m_offset + 9, str.c_str(), srcLength);
        for (std::size_t start = srcLength; start < 24; ++start)
        {
            m_buffer[m_offset + 9 + start] = 0;
        }

        return *this;
    }
    #endif

    SBE_NODISCARD static const char *seqNumMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
    {
        switch (metaAttribute)
        {
            case MetaAttribute::PRESENCE: return "required";
            default: return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t seqNumId() SBE_NOEXCEPT
    {
        return 4;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t seqNumSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool seqNumInActingVersion() SBE_NOEXCEPT
    {
        return true;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t seqNumEncodingOffset() SBE_NOEXCEPT
    {
        return 33;
    }

    static SBE_CONSTEXPR std::uint64_t seqNumNullValue() SBE_NOEXCEPT
    {
        return SBE_NULLVALUE_UINT64;
    }

    static SBE_CONSTEXPR std::uint64_t seqNumMinValue() SBE_NOEXCEPT
    {
        return UINT64_C(0x0);
    }

    static SBE_CONSTEXPR std::uint64_t seqNumMaxValue() SBE_NOEXCEPT
    {
        return UINT64_C(0xfffffffffffffffe);
    }

    static SBE_CONSTEXPR std::size_t seqNumEncodingLength() SBE_NOEXCEPT
    {
        return 8;
    }

    SBE_NODISCARD std::uint64_t seqNum() const SBE_NOEXCEPT
    {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 33, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    VolSurface &seqNum(const std::uint64_t value) SBE_NOEXCEPT
    {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 33, &val, sizeof(std::uint64_t));
        return *this;
    }

    class Points
    {
    private:
        char *m_buffer = nullptr;
        std::uint64_t m_bufferLength = 0;
        std::uint64_t m_initialPosition = 0;
        std::uint64_t *m_positionPtr = nullptr;
        std::uint64_t m_blockLength = 0;
        std::uint64_t m_count = 0;
        std::uint64_t m_index = 0;
        std::uint64_t m_offset = 0;
        std::uint64_t m_actingVersion = 0;

        SBE_NODISCARD std::uint64_t *sbePositionPtr() SBE_NOEXCEPT
        {
            return m_positionPtr;
        }

    public:
        Points() = default;

        inline void wrapForDecode(
            char *buffer,
            std::uint64_t *pos,
            const std::uint64_t actingVersion,
            const std::uint64_t bufferLength)
        {
            GroupSizeEncoding dimensions(buffer, *pos, bufferLength, actingVersion);
            m_buffer = buffer;
            m_bufferLength = bufferLength;
            m_blockLength = dimensions.blockLength();
            m_count = dimensions.numInGroup();
            m_index = 0;
            m_actingVersion = actingVersion;
            m_initialPosition = *pos;
            m_positionPtr = pos;
            *m_positionPtr = *m_positionPtr + 4;
        }

        inline void wrapForEncode(
            char *buffer,
            const std::uint16_t count,
            std::uint64_t *pos,
            const std::uint64_t actingVersion,
            const std::uint64_t bufferLength)
        {
    #if defined(__GNUG__) && !defined(__clang__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wtype-limits"
    #endif
            if (count > 65534)
            {
                throw std::runtime_error("count outside of allowed range [E110]");
            }
    #if defined(__GNUG__) && !defined(__clang__)
    #pragma GCC diagnostic pop
    #endif
            m_buffer = buffer;
            m_bufferLength = bufferLength;
            GroupSizeEncoding dimensions(buffer, *pos, bufferLength, actingVersion);
            dimensions.blockLength(static_cast<std::uint16_t>(109));
            dimensions.numInGroup(static_cast<std::uint16_t>(count));
            m_index = 0;
            m_count = count;
            m_blockLength = 109;
            m_actingVersion = actingVersion;
            m_initialPosition = *pos;
            m_positionPtr = pos;
            *m_positionPtr = *m_positionPtr + 4;
        }

        static SBE_CONSTEXPR std::uint64_t sbeHeaderSize() SBE_NOEXCEPT
        {
            return 4;
        }

        static SBE_CONSTEXPR std::uint64_t sbeBlockLength() SBE_NOEXCEPT
        {
            return 109;
        }

        SBE_NODISCARD std::uint64_t sbePosition() const SBE_NOEXCEPT
        {
            return *m_positionPtr;
        }

        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        std::uint64_t sbeCheckPosition(const std::uint64_t position)
        {
            if (SBE_BOUNDS_CHECK_EXPECT((position > m_bufferLength), false))
            {
                throw std::runtime_error("buffer too short [E100]");
            }
            return position;
        }

        void sbePosition(const std::uint64_t position)
        {
            *m_positionPtr = sbeCheckPosition(position);
        }

        SBE_NODISCARD inline std::uint64_t count() const SBE_NOEXCEPT
        {
            return m_count;
        }

        SBE_NODISCARD inline bool hasNext() const SBE_NOEXCEPT
        {
            return m_index < m_count;
        }

        inline Points &next()
        {
            if (m_index >= m_count)
            {
                throw std::runtime_error("index >= count [E108]");
            }
            m_offset = *m_positionPtr;
            if (SBE_BOUNDS_CHECK_EXPECT(((m_offset + m_blockLength) > m_bufferLength), false))
            {
                throw std::runtime_error("buffer too short for next group index [E108]");
            }
            *m_positionPtr = m_offset + m_blockLength;
            ++m_index;

            return *this;
        }

        inline std::uint64_t resetCountToIndex()
        {
            m_count = m_index;
            GroupSizeEncoding dimensions(m_buffer, m_initialPosition, m_bufferLength, m_actingVersion);
            dimensions.numInGroup(static_cast<std::uint16_t>(m_count));
            return m_count;
        }

        template<class Func> inline void forEach(Func &&func)
        {
            while (hasNext())
            {
                next();
                func(*this);
            }
        }


        SBE_NODISCARD static const char *instrumentIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t instrumentIdId() SBE_NOEXCEPT
        {
            return 1;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t instrumentIdSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool instrumentIdInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t instrumentIdEncodingOffset() SBE_NOEXCEPT
        {
            return 0;
        }

        static SBE_CONSTEXPR std::uint64_t instrumentIdNullValue() SBE_NOEXCEPT
        {
            return SBE_NULLVALUE_UINT64;
        }

        static SBE_CONSTEXPR std::uint64_t instrumentIdMinValue() SBE_NOEXCEPT
        {
            return UINT64_C(0x0);
        }

        static SBE_CONSTEXPR std::uint64_t instrumentIdMaxValue() SBE_NOEXCEPT
        {
            return UINT64_C(0xfffffffffffffffe);
        }

        static SBE_CONSTEXPR std::size_t instrumentIdEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD std::uint64_t instrumentId() const SBE_NOEXCEPT
        {
            std::uint64_t val;
            std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint64_t));
            return SBE_LITTLE_ENDIAN_ENCODE_64(val);
        }

        Points &instrumentId(const std::uint64_t value) SBE_NOEXCEPT
        {
            std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
            std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::uint64_t));
            return *this;
        }

        SBE_NODISCARD static const char *expiryDateMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t expiryDateId() SBE_NOEXCEPT
        {
            return 2;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t expiryDateSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool expiryDateInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t expiryDateEncodingOffset() SBE_NOEXCEPT
        {
            return 8;
        }

        static SBE_CONSTEXPR std::uint32_t expiryDateNullValue() SBE_NOEXCEPT
        {
            return SBE_NULLVALUE_UINT32;
        }

        static SBE_CONSTEXPR std::uint32_t expiryDateMinValue() SBE_NOEXCEPT
        {
            return UINT32_C(0x0);
        }

        static SBE_CONSTEXPR std::uint32_t expiryDateMaxValue() SBE_NOEXCEPT
        {
            return UINT32_C(0xfffffffe);
        }

        static SBE_CONSTEXPR std::size_t expiryDateEncodingLength() SBE_NOEXCEPT
        {
            return 4;
        }

        SBE_NODISCARD std::uint32_t expiryDate() const SBE_NOEXCEPT
        {
            std::uint32_t val;
            std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint32_t));
            return SBE_LITTLE_ENDIAN_ENCODE_32(val);
        }

        Points &expiryDate(const std::uint32_t value) SBE_NOEXCEPT
        {
            std::uint32_t val = SBE_LITTLE_ENDIAN_ENCODE_32(value);
            std::memcpy(m_buffer + m_offset + 8, &val, sizeof(std::uint32_t));
            return *this;
        }

        SBE_NODISCARD static const char *strikePriceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t strikePriceId() SBE_NOEXCEPT
        {
            return 3;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t strikePriceSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool strikePriceInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t strikePriceEncodingOffset() SBE_NOEXCEPT
        {
            return 12;
        }

        static SBE_CONSTEXPR double strikePriceNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double strikePriceMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double strikePriceMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t strikePriceEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double strikePrice() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 12, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &strikePrice(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 12, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *optionSideMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t optionSideId() SBE_NOEXCEPT
        {
            return 4;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t optionSideSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool optionSideInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t optionSideEncodingOffset() SBE_NOEXCEPT
        {
            return 20;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t optionSideEncodingLength() SBE_NOEXCEPT
        {
            return 1;
        }

        SBE_NODISCARD std::uint8_t optionSideRaw() const SBE_NOEXCEPT
        {
            std::uint8_t val;
            std::memcpy(&val, m_buffer + m_offset + 20, sizeof(std::uint8_t));
            return (val);
        }

        SBE_NODISCARD OptionSide::Value optionSide() const
        {
            std::uint8_t val;
            std::memcpy(&val, m_buffer + m_offset + 20, sizeof(std::uint8_t));
            return OptionSide::get((val));
        }

        Points &optionSide(const OptionSide::Value value) SBE_NOEXCEPT
        {
            std::uint8_t val = (value);
            std::memcpy(m_buffer + m_offset + 20, &val, sizeof(std::uint8_t));
            return *this;
        }

        SBE_NODISCARD static const char *impliedVolMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t impliedVolId() SBE_NOEXCEPT
        {
            return 5;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t impliedVolSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool impliedVolInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t impliedVolEncodingOffset() SBE_NOEXCEPT
        {
            return 21;
        }

        static SBE_CONSTEXPR double impliedVolNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double impliedVolMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double impliedVolMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t impliedVolEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double impliedVol() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 21, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &impliedVol(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 21, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *forwardPriceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t forwardPriceId() SBE_NOEXCEPT
        {
            return 6;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t forwardPriceSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool forwardPriceInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t forwardPriceEncodingOffset() SBE_NOEXCEPT
        {
            return 29;
        }

        static SBE_CONSTEXPR double forwardPriceNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double forwardPriceMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double forwardPriceMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t forwardPriceEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double forwardPrice() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 29, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &forwardPrice(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 29, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *timeToExpiryMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t timeToExpiryId() SBE_NOEXCEPT
        {
            return 7;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t timeToExpirySinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool timeToExpiryInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t timeToExpiryEncodingOffset() SBE_NOEXCEPT
        {
            return 37;
        }

        static SBE_CONSTEXPR double timeToExpiryNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double timeToExpiryMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double timeToExpiryMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t timeToExpiryEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double timeToExpiry() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 37, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &timeToExpiry(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 37, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *bidIvMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t bidIvId() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t bidIvSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool bidIvInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t bidIvEncodingOffset() SBE_NOEXCEPT
        {
            return 45;
        }

        static SBE_CONSTEXPR double bidIvNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double bidIvMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double bidIvMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t bidIvEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double bidIv() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 45, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &bidIv(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 45, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *askIvMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t askIvId() SBE_NOEXCEPT
        {
            return 9;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t askIvSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool askIvInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t askIvEncodingOffset() SBE_NOEXCEPT
        {
            return 53;
        }

        static SBE_CONSTEXPR double askIvNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double askIvMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double askIvMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t askIvEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double askIv() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 53, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &askIv(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 53, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *bidPriceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t bidPriceId() SBE_NOEXCEPT
        {
            return 10;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t bidPriceSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool bidPriceInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t bidPriceEncodingOffset() SBE_NOEXCEPT
        {
            return 61;
        }

        static SBE_CONSTEXPR double bidPriceNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double bidPriceMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double bidPriceMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t bidPriceEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double bidPrice() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 61, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &bidPrice(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 61, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *askPriceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t askPriceId() SBE_NOEXCEPT
        {
            return 11;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t askPriceSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool askPriceInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t askPriceEncodingOffset() SBE_NOEXCEPT
        {
            return 69;
        }

        static SBE_CONSTEXPR double askPriceNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double askPriceMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double askPriceMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t askPriceEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double askPrice() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 69, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &askPrice(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 69, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *deltaMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t deltaId() SBE_NOEXCEPT
        {
            return 12;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t deltaSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool deltaInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t deltaEncodingOffset() SBE_NOEXCEPT
        {
            return 77;
        }

        static SBE_CONSTEXPR double deltaNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double deltaMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double deltaMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t deltaEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double delta() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 77, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &delta(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 77, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *gammaMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t gammaId() SBE_NOEXCEPT
        {
            return 13;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t gammaSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool gammaInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t gammaEncodingOffset() SBE_NOEXCEPT
        {
            return 85;
        }

        static SBE_CONSTEXPR double gammaNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double gammaMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double gammaMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t gammaEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double gamma() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 85, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &gamma(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 85, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *vegaMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t vegaId() SBE_NOEXCEPT
        {
            return 14;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t vegaSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool vegaInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t vegaEncodingOffset() SBE_NOEXCEPT
        {
            return 93;
        }

        static SBE_CONSTEXPR double vegaNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double vegaMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double vegaMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t vegaEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double vega() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 93, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &vega(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 93, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char *thetaMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t thetaId() SBE_NOEXCEPT
        {
            return 15;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t thetaSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool thetaInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t thetaEncodingOffset() SBE_NOEXCEPT
        {
            return 101;
        }

        static SBE_CONSTEXPR double thetaNullValue() SBE_NOEXCEPT
        {
            return SBE_DOUBLE_NAN;
        }

        static SBE_CONSTEXPR double thetaMinValue() SBE_NOEXCEPT
        {
            return 4.9E-324;
        }

        static SBE_CONSTEXPR double thetaMaxValue() SBE_NOEXCEPT
        {
            return 1.7976931348623157E308;
        }

        static SBE_CONSTEXPR std::size_t thetaEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD double theta() const SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 101, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Points &theta(const double value) SBE_NOEXCEPT
        {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 101, &val, sizeof(double));
            return *this;
        }

        template<typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits> & operator << (
            std::basic_ostream<CharT, Traits> &builder, Points &writer)
        {
            builder << '{';
            builder << R"("instrumentId": )";
            builder << +writer.instrumentId();

            builder << ", ";
            builder << R"("expiryDate": )";
            builder << +writer.expiryDate();

            builder << ", ";
            builder << R"("strikePrice": )";
            builder << +writer.strikePrice();

            builder << ", ";
            builder << R"("optionSide": )";
            builder << '"' << writer.optionSide() << '"';

            builder << ", ";
            builder << R"("impliedVol": )";
            builder << +writer.impliedVol();

            builder << ", ";
            builder << R"("forwardPrice": )";
            builder << +writer.forwardPrice();

            builder << ", ";
            builder << R"("timeToExpiry": )";
            builder << +writer.timeToExpiry();

            builder << ", ";
            builder << R"("bidIv": )";
            builder << +writer.bidIv();

            builder << ", ";
            builder << R"("askIv": )";
            builder << +writer.askIv();

            builder << ", ";
            builder << R"("bidPrice": )";
            builder << +writer.bidPrice();

            builder << ", ";
            builder << R"("askPrice": )";
            builder << +writer.askPrice();

            builder << ", ";
            builder << R"("delta": )";
            builder << +writer.delta();

            builder << ", ";
            builder << R"("gamma": )";
            builder << +writer.gamma();

            builder << ", ";
            builder << R"("vega": )";
            builder << +writer.vega();

            builder << ", ";
            builder << R"("theta": )";
            builder << +writer.theta();

            builder << '}';

            return builder;
        }

        void skip()
        {
        }

        SBE_NODISCARD static SBE_CONSTEXPR bool isConstLength() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static std::size_t computeLength()
        {
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
            std::size_t length = sbeBlockLength();

            return length;
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
        }
    };

private:
    Points m_points;

public:
    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t pointsId() SBE_NOEXCEPT
    {
        return 5;
    }

    SBE_NODISCARD inline Points &points()
    {
        m_points.wrapForDecode(m_buffer, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_points;
    }

    Points &pointsCount(const std::uint16_t count)
    {
        m_points.wrapForEncode(m_buffer, count, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_points;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t pointsSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool pointsInActingVersion() const SBE_NOEXCEPT
    {
        return true;
    }

template<typename CharT, typename Traits>
friend std::basic_ostream<CharT, Traits> & operator << (
    std::basic_ostream<CharT, Traits> &builder, const VolSurface &_writer)
{
    VolSurface writer(
        _writer.m_buffer,
        _writer.m_offset,
        _writer.m_bufferLength,
        _writer.m_actingBlockLength,
        _writer.m_actingVersion);

    builder << '{';
    builder << R"("Name": "VolSurface", )";
    builder << R"("sbeTemplateId": )";
    builder << writer.sbeTemplateId();
    builder << ", ";

    builder << R"("timestampNs": )";
    builder << +writer.timestampNs();

    builder << ", ";
    builder << R"("exchangeId": )";
    builder << '"' << writer.exchangeId() << '"';

    builder << ", ";
    builder << R"("underlying": )";
    builder << '"' <<
        writer.getUnderlyingAsJsonEscapedString().c_str() << '"';

    builder << ", ";
    builder << R"("seqNum": )";
    builder << +writer.seqNum();

    builder << ", ";
    {
        bool atLeastOne = false;
        builder << R"("points": [)";
        writer.points().forEach(
            [&](Points &points)
            {
                if (atLeastOne)
                {
                    builder << ", ";
                }
                atLeastOne = true;
                builder << points;
            });
        builder << ']';
    }

    builder << '}';

    return builder;
}

void skip()
{
    auto &pointsGroup { points() };
    while (pointsGroup.hasNext())
    {
        pointsGroup.next().skip();
    }
}

SBE_NODISCARD static SBE_CONSTEXPR bool isConstLength() SBE_NOEXCEPT
{
    return false;
}

SBE_NODISCARD static std::size_t computeLength(std::size_t pointsLength = 0)
{
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
    std::size_t length = sbeBlockLength();

    length += Points::sbeHeaderSize();
    if (pointsLength > 65534LL)
    {
        throw std::runtime_error("pointsLength outside of allowed range [E110]");
    }
    length += pointsLength *Points::sbeBlockLength();

    return length;
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}
};
}
}
#endif

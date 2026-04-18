/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BPT_MESSAGES_REFDATASUBSCRIPTIONREQUEST_CXX_H_
#define _BPT_MESSAGES_REFDATASUBSCRIPTIONREQUEST_CXX_H_

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

class RefDataSubscriptionRequest
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
    static const std::uint16_t SBE_BLOCK_LENGTH = static_cast<std::uint16_t>(16);
    static const std::uint16_t SBE_TEMPLATE_ID = static_cast<std::uint16_t>(1);
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

    RefDataSubscriptionRequest() = default;

    RefDataSubscriptionRequest(
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

    RefDataSubscriptionRequest(char *buffer, const std::uint64_t bufferLength) :
        RefDataSubscriptionRequest(buffer, 0, bufferLength, sbeBlockLength(), sbeSchemaVersion())
    {
    }

    RefDataSubscriptionRequest(
        char *buffer,
        const std::uint64_t bufferLength,
        const std::uint64_t actingBlockLength,
        const std::uint64_t actingVersion) :
        RefDataSubscriptionRequest(buffer, 0, bufferLength, actingBlockLength, actingVersion)
    {
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeBlockLength() SBE_NOEXCEPT
    {
        return static_cast<std::uint16_t>(16);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sbeBlockAndHeaderLength() SBE_NOEXCEPT
    {
        return messageHeader::encodedLength() + sbeBlockLength();
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeTemplateId() SBE_NOEXCEPT
    {
        return static_cast<std::uint16_t>(1);
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

    RefDataSubscriptionRequest &wrapForEncode(char *buffer, const std::uint64_t offset, const std::uint64_t bufferLength)
    {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    RefDataSubscriptionRequest &wrapAndApplyHeader(char *buffer, const std::uint64_t offset, const std::uint64_t bufferLength)
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

    RefDataSubscriptionRequest &wrapForDecode(
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

    RefDataSubscriptionRequest &sbeRewind()
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
        RefDataSubscriptionRequest skipper(m_buffer, m_offset, m_bufferLength, sbeBlockLength(), m_actingVersion);
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

    SBE_NODISCARD static const char *correlationIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
    {
        switch (metaAttribute)
        {
            case MetaAttribute::PRESENCE: return "required";
            default: return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t correlationIdId() SBE_NOEXCEPT
    {
        return 1;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t correlationIdSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool correlationIdInActingVersion() SBE_NOEXCEPT
    {
        return true;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t correlationIdEncodingOffset() SBE_NOEXCEPT
    {
        return 0;
    }

    static SBE_CONSTEXPR std::uint64_t correlationIdNullValue() SBE_NOEXCEPT
    {
        return SBE_NULLVALUE_UINT64;
    }

    static SBE_CONSTEXPR std::uint64_t correlationIdMinValue() SBE_NOEXCEPT
    {
        return UINT64_C(0x0);
    }

    static SBE_CONSTEXPR std::uint64_t correlationIdMaxValue() SBE_NOEXCEPT
    {
        return UINT64_C(0xfffffffffffffffe);
    }

    static SBE_CONSTEXPR std::size_t correlationIdEncodingLength() SBE_NOEXCEPT
    {
        return 8;
    }

    SBE_NODISCARD std::uint64_t correlationId() const SBE_NOEXCEPT
    {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    RefDataSubscriptionRequest &correlationId(const std::uint64_t value) SBE_NOEXCEPT
    {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::uint64_t));
        return *this;
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
        return 2;
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
        return 8;
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
        std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    RefDataSubscriptionRequest &timestampNs(const std::uint64_t value) SBE_NOEXCEPT
    {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 8, &val, sizeof(std::uint64_t));
        return *this;
    }

    class Instruments
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
        Instruments() = default;

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
            dimensions.blockLength(static_cast<std::uint16_t>(32));
            dimensions.numInGroup(static_cast<std::uint16_t>(count));
            m_index = 0;
            m_count = count;
            m_blockLength = 32;
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
            return 32;
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

        inline Instruments &next()
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


        SBE_NODISCARD static const char *symbolMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t symbolId() SBE_NOEXCEPT
        {
            return 1;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t symbolSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool symbolInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t symbolEncodingOffset() SBE_NOEXCEPT
        {
            return 0;
        }

        static SBE_CONSTEXPR char symbolNullValue() SBE_NOEXCEPT
        {
            return static_cast<char>(0);
        }

        static SBE_CONSTEXPR char symbolMinValue() SBE_NOEXCEPT
        {
            return static_cast<char>(32);
        }

        static SBE_CONSTEXPR char symbolMaxValue() SBE_NOEXCEPT
        {
            return static_cast<char>(126);
        }

        static SBE_CONSTEXPR std::size_t symbolEncodingLength() SBE_NOEXCEPT
        {
            return 24;
        }

        static SBE_CONSTEXPR std::uint64_t symbolLength() SBE_NOEXCEPT
        {
            return 24;
        }

        SBE_NODISCARD const char *symbol() const SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 0;
        }

        SBE_NODISCARD char *symbol() SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 0;
        }

        SBE_NODISCARD char symbol(const std::uint64_t index) const
        {
            if (index >= 24)
            {
                throw std::runtime_error("index out of range for symbol [E104]");
            }

            char val;
            std::memcpy(&val, m_buffer + m_offset + 0 + (index * 1), sizeof(char));
            return (val);
        }

        Instruments &symbol(const std::uint64_t index, const char value)
        {
            if (index >= 24)
            {
                throw std::runtime_error("index out of range for symbol [E105]");
            }

            char val = (value);
            std::memcpy(m_buffer + m_offset + 0 + (index * 1), &val, sizeof(char));
            return *this;
        }

        std::uint64_t getSymbol(char *const dst, const std::uint64_t length) const
        {
            if (length > 24)
            {
                throw std::runtime_error("length too large for getSymbol [E106]");
            }

            std::memcpy(dst, m_buffer + m_offset + 0, sizeof(char) * static_cast<std::size_t>(length));
            return length;
        }

        Instruments &putSymbol(const char *const src) SBE_NOEXCEPT
        {
            std::memcpy(m_buffer + m_offset + 0, src, sizeof(char) * 24);
            return *this;
        }

        SBE_NODISCARD std::string getSymbolAsString() const
        {
            const char *buffer = m_buffer + m_offset + 0;
            std::size_t length = 0;

            for (; length < 24 && *(buffer + length) != '\0'; ++length);
            std::string result(buffer, length);

            return result;
        }

        std::string getSymbolAsJsonEscapedString()
        {
            std::ostringstream oss;
            std::string s = getSymbolAsString();

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
        SBE_NODISCARD std::string_view getSymbolAsStringView() const SBE_NOEXCEPT
        {
            const char *buffer = m_buffer + m_offset + 0;
            std::size_t length = 0;

            for (; length < 24 && *(buffer + length) != '\0'; ++length);
            std::string_view result(buffer, length);

            return result;
        }
        #endif

        #if __cplusplus >= 201703L
        Instruments &putSymbol(const std::string_view str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 24)
            {
                throw std::runtime_error("string too large for putSymbol [E106]");
            }

            std::memcpy(m_buffer + m_offset + 0, str.data(), srcLength);
            for (std::size_t start = srcLength; start < 24; ++start)
            {
                m_buffer[m_offset + 0 + start] = 0;
            }

            return *this;
        }
        #else
        Instruments &putSymbol(const std::string &str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 24)
            {
                throw std::runtime_error("string too large for putSymbol [E106]");
            }

            std::memcpy(m_buffer + m_offset + 0, str.c_str(), srcLength);
            for (std::size_t start = srcLength; start < 24; ++start)
            {
                m_buffer[m_offset + 0 + start] = 0;
            }

            return *this;
        }
        #endif

        SBE_NODISCARD static const char *exchangeMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t exchangeId() SBE_NOEXCEPT
        {
            return 2;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool exchangeInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeEncodingOffset() SBE_NOEXCEPT
        {
            return 24;
        }

        static SBE_CONSTEXPR char exchangeNullValue() SBE_NOEXCEPT
        {
            return static_cast<char>(0);
        }

        static SBE_CONSTEXPR char exchangeMinValue() SBE_NOEXCEPT
        {
            return static_cast<char>(32);
        }

        static SBE_CONSTEXPR char exchangeMaxValue() SBE_NOEXCEPT
        {
            return static_cast<char>(126);
        }

        static SBE_CONSTEXPR std::size_t exchangeEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        static SBE_CONSTEXPR std::uint64_t exchangeLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD const char *exchange() const SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 24;
        }

        SBE_NODISCARD char *exchange() SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 24;
        }

        SBE_NODISCARD char exchange(const std::uint64_t index) const
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for exchange [E104]");
            }

            char val;
            std::memcpy(&val, m_buffer + m_offset + 24 + (index * 1), sizeof(char));
            return (val);
        }

        Instruments &exchange(const std::uint64_t index, const char value)
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for exchange [E105]");
            }

            char val = (value);
            std::memcpy(m_buffer + m_offset + 24 + (index * 1), &val, sizeof(char));
            return *this;
        }

        std::uint64_t getExchange(char *const dst, const std::uint64_t length) const
        {
            if (length > 8)
            {
                throw std::runtime_error("length too large for getExchange [E106]");
            }

            std::memcpy(dst, m_buffer + m_offset + 24, sizeof(char) * static_cast<std::size_t>(length));
            return length;
        }

        Instruments &putExchange(const char *const src) SBE_NOEXCEPT
        {
            std::memcpy(m_buffer + m_offset + 24, src, sizeof(char) * 8);
            return *this;
        }

        SBE_NODISCARD std::string getExchangeAsString() const
        {
            const char *buffer = m_buffer + m_offset + 24;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string result(buffer, length);

            return result;
        }

        std::string getExchangeAsJsonEscapedString()
        {
            std::ostringstream oss;
            std::string s = getExchangeAsString();

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
        SBE_NODISCARD std::string_view getExchangeAsStringView() const SBE_NOEXCEPT
        {
            const char *buffer = m_buffer + m_offset + 24;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string_view result(buffer, length);

            return result;
        }
        #endif

        #if __cplusplus >= 201703L
        Instruments &putExchange(const std::string_view str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putExchange [E106]");
            }

            std::memcpy(m_buffer + m_offset + 24, str.data(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 24 + start] = 0;
            }

            return *this;
        }
        #else
        Instruments &putExchange(const std::string &str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putExchange [E106]");
            }

            std::memcpy(m_buffer + m_offset + 24, str.c_str(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 24 + start] = 0;
            }

            return *this;
        }
        #endif

        template<typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits> & operator << (
            std::basic_ostream<CharT, Traits> &builder, Instruments &writer)
        {
            builder << '{';
            builder << R"("symbol": )";
            builder << '"' <<
                writer.getSymbolAsJsonEscapedString().c_str() << '"';

            builder << ", ";
            builder << R"("exchange": )";
            builder << '"' <<
                writer.getExchangeAsJsonEscapedString().c_str() << '"';

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
    Instruments m_instruments;

public:
    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t instrumentsId() SBE_NOEXCEPT
    {
        return 3;
    }

    SBE_NODISCARD inline Instruments &instruments()
    {
        m_instruments.wrapForDecode(m_buffer, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_instruments;
    }

    Instruments &instrumentsCount(const std::uint16_t count)
    {
        m_instruments.wrapForEncode(m_buffer, count, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_instruments;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t instrumentsSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool instrumentsInActingVersion() const SBE_NOEXCEPT
    {
        return true;
    }

    class CanonicalFilter
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
        CanonicalFilter() = default;

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
            dimensions.blockLength(static_cast<std::uint16_t>(25));
            dimensions.numInGroup(static_cast<std::uint16_t>(count));
            m_index = 0;
            m_count = count;
            m_blockLength = 25;
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
            return 25;
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

        inline CanonicalFilter &next()
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


        SBE_NODISCARD static const char *baseCurrencyMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t baseCurrencyId() SBE_NOEXCEPT
        {
            return 1;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t baseCurrencySinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool baseCurrencyInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t baseCurrencyEncodingOffset() SBE_NOEXCEPT
        {
            return 0;
        }

        static SBE_CONSTEXPR char baseCurrencyNullValue() SBE_NOEXCEPT
        {
            return static_cast<char>(0);
        }

        static SBE_CONSTEXPR char baseCurrencyMinValue() SBE_NOEXCEPT
        {
            return static_cast<char>(32);
        }

        static SBE_CONSTEXPR char baseCurrencyMaxValue() SBE_NOEXCEPT
        {
            return static_cast<char>(126);
        }

        static SBE_CONSTEXPR std::size_t baseCurrencyEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        static SBE_CONSTEXPR std::uint64_t baseCurrencyLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD const char *baseCurrency() const SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 0;
        }

        SBE_NODISCARD char *baseCurrency() SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 0;
        }

        SBE_NODISCARD char baseCurrency(const std::uint64_t index) const
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for baseCurrency [E104]");
            }

            char val;
            std::memcpy(&val, m_buffer + m_offset + 0 + (index * 1), sizeof(char));
            return (val);
        }

        CanonicalFilter &baseCurrency(const std::uint64_t index, const char value)
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for baseCurrency [E105]");
            }

            char val = (value);
            std::memcpy(m_buffer + m_offset + 0 + (index * 1), &val, sizeof(char));
            return *this;
        }

        std::uint64_t getBaseCurrency(char *const dst, const std::uint64_t length) const
        {
            if (length > 8)
            {
                throw std::runtime_error("length too large for getBaseCurrency [E106]");
            }

            std::memcpy(dst, m_buffer + m_offset + 0, sizeof(char) * static_cast<std::size_t>(length));
            return length;
        }

        CanonicalFilter &putBaseCurrency(const char *const src) SBE_NOEXCEPT
        {
            std::memcpy(m_buffer + m_offset + 0, src, sizeof(char) * 8);
            return *this;
        }

        SBE_NODISCARD std::string getBaseCurrencyAsString() const
        {
            const char *buffer = m_buffer + m_offset + 0;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string result(buffer, length);

            return result;
        }

        std::string getBaseCurrencyAsJsonEscapedString()
        {
            std::ostringstream oss;
            std::string s = getBaseCurrencyAsString();

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
        SBE_NODISCARD std::string_view getBaseCurrencyAsStringView() const SBE_NOEXCEPT
        {
            const char *buffer = m_buffer + m_offset + 0;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string_view result(buffer, length);

            return result;
        }
        #endif

        #if __cplusplus >= 201703L
        CanonicalFilter &putBaseCurrency(const std::string_view str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putBaseCurrency [E106]");
            }

            std::memcpy(m_buffer + m_offset + 0, str.data(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 0 + start] = 0;
            }

            return *this;
        }
        #else
        CanonicalFilter &putBaseCurrency(const std::string &str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putBaseCurrency [E106]");
            }

            std::memcpy(m_buffer + m_offset + 0, str.c_str(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 0 + start] = 0;
            }

            return *this;
        }
        #endif

        SBE_NODISCARD static const char *quoteCurrencyMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t quoteCurrencyId() SBE_NOEXCEPT
        {
            return 2;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t quoteCurrencySinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool quoteCurrencyInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t quoteCurrencyEncodingOffset() SBE_NOEXCEPT
        {
            return 8;
        }

        static SBE_CONSTEXPR char quoteCurrencyNullValue() SBE_NOEXCEPT
        {
            return static_cast<char>(0);
        }

        static SBE_CONSTEXPR char quoteCurrencyMinValue() SBE_NOEXCEPT
        {
            return static_cast<char>(32);
        }

        static SBE_CONSTEXPR char quoteCurrencyMaxValue() SBE_NOEXCEPT
        {
            return static_cast<char>(126);
        }

        static SBE_CONSTEXPR std::size_t quoteCurrencyEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        static SBE_CONSTEXPR std::uint64_t quoteCurrencyLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD const char *quoteCurrency() const SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 8;
        }

        SBE_NODISCARD char *quoteCurrency() SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 8;
        }

        SBE_NODISCARD char quoteCurrency(const std::uint64_t index) const
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for quoteCurrency [E104]");
            }

            char val;
            std::memcpy(&val, m_buffer + m_offset + 8 + (index * 1), sizeof(char));
            return (val);
        }

        CanonicalFilter &quoteCurrency(const std::uint64_t index, const char value)
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for quoteCurrency [E105]");
            }

            char val = (value);
            std::memcpy(m_buffer + m_offset + 8 + (index * 1), &val, sizeof(char));
            return *this;
        }

        std::uint64_t getQuoteCurrency(char *const dst, const std::uint64_t length) const
        {
            if (length > 8)
            {
                throw std::runtime_error("length too large for getQuoteCurrency [E106]");
            }

            std::memcpy(dst, m_buffer + m_offset + 8, sizeof(char) * static_cast<std::size_t>(length));
            return length;
        }

        CanonicalFilter &putQuoteCurrency(const char *const src) SBE_NOEXCEPT
        {
            std::memcpy(m_buffer + m_offset + 8, src, sizeof(char) * 8);
            return *this;
        }

        SBE_NODISCARD std::string getQuoteCurrencyAsString() const
        {
            const char *buffer = m_buffer + m_offset + 8;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string result(buffer, length);

            return result;
        }

        std::string getQuoteCurrencyAsJsonEscapedString()
        {
            std::ostringstream oss;
            std::string s = getQuoteCurrencyAsString();

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
        SBE_NODISCARD std::string_view getQuoteCurrencyAsStringView() const SBE_NOEXCEPT
        {
            const char *buffer = m_buffer + m_offset + 8;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string_view result(buffer, length);

            return result;
        }
        #endif

        #if __cplusplus >= 201703L
        CanonicalFilter &putQuoteCurrency(const std::string_view str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putQuoteCurrency [E106]");
            }

            std::memcpy(m_buffer + m_offset + 8, str.data(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 8 + start] = 0;
            }

            return *this;
        }
        #else
        CanonicalFilter &putQuoteCurrency(const std::string &str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putQuoteCurrency [E106]");
            }

            std::memcpy(m_buffer + m_offset + 8, str.c_str(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 8 + start] = 0;
            }

            return *this;
        }
        #endif

        SBE_NODISCARD static const char *instrumentTypeMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t instrumentTypeId() SBE_NOEXCEPT
        {
            return 3;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t instrumentTypeSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool instrumentTypeInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t instrumentTypeEncodingOffset() SBE_NOEXCEPT
        {
            return 16;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t instrumentTypeEncodingLength() SBE_NOEXCEPT
        {
            return 1;
        }

        SBE_NODISCARD std::uint8_t instrumentTypeRaw() const SBE_NOEXCEPT
        {
            std::uint8_t val;
            std::memcpy(&val, m_buffer + m_offset + 16, sizeof(std::uint8_t));
            return (val);
        }

        SBE_NODISCARD InstrumentType::Value instrumentType() const
        {
            std::uint8_t val;
            std::memcpy(&val, m_buffer + m_offset + 16, sizeof(std::uint8_t));
            return InstrumentType::get((val));
        }

        CanonicalFilter &instrumentType(const InstrumentType::Value value) SBE_NOEXCEPT
        {
            std::uint8_t val = (value);
            std::memcpy(m_buffer + m_offset + 16, &val, sizeof(std::uint8_t));
            return *this;
        }

        SBE_NODISCARD static const char *exchangeMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT
        {
            switch (metaAttribute)
            {
                case MetaAttribute::PRESENCE: return "required";
                default: return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t exchangeId() SBE_NOEXCEPT
        {
            return 4;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeSinceVersion() SBE_NOEXCEPT
        {
            return 0;
        }

        SBE_NODISCARD bool exchangeInActingVersion() SBE_NOEXCEPT
        {
            return true;
        }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeEncodingOffset() SBE_NOEXCEPT
        {
            return 17;
        }

        static SBE_CONSTEXPR char exchangeNullValue() SBE_NOEXCEPT
        {
            return static_cast<char>(0);
        }

        static SBE_CONSTEXPR char exchangeMinValue() SBE_NOEXCEPT
        {
            return static_cast<char>(32);
        }

        static SBE_CONSTEXPR char exchangeMaxValue() SBE_NOEXCEPT
        {
            return static_cast<char>(126);
        }

        static SBE_CONSTEXPR std::size_t exchangeEncodingLength() SBE_NOEXCEPT
        {
            return 8;
        }

        static SBE_CONSTEXPR std::uint64_t exchangeLength() SBE_NOEXCEPT
        {
            return 8;
        }

        SBE_NODISCARD const char *exchange() const SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 17;
        }

        SBE_NODISCARD char *exchange() SBE_NOEXCEPT
        {
            return m_buffer + m_offset + 17;
        }

        SBE_NODISCARD char exchange(const std::uint64_t index) const
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for exchange [E104]");
            }

            char val;
            std::memcpy(&val, m_buffer + m_offset + 17 + (index * 1), sizeof(char));
            return (val);
        }

        CanonicalFilter &exchange(const std::uint64_t index, const char value)
        {
            if (index >= 8)
            {
                throw std::runtime_error("index out of range for exchange [E105]");
            }

            char val = (value);
            std::memcpy(m_buffer + m_offset + 17 + (index * 1), &val, sizeof(char));
            return *this;
        }

        std::uint64_t getExchange(char *const dst, const std::uint64_t length) const
        {
            if (length > 8)
            {
                throw std::runtime_error("length too large for getExchange [E106]");
            }

            std::memcpy(dst, m_buffer + m_offset + 17, sizeof(char) * static_cast<std::size_t>(length));
            return length;
        }

        CanonicalFilter &putExchange(const char *const src) SBE_NOEXCEPT
        {
            std::memcpy(m_buffer + m_offset + 17, src, sizeof(char) * 8);
            return *this;
        }

        SBE_NODISCARD std::string getExchangeAsString() const
        {
            const char *buffer = m_buffer + m_offset + 17;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string result(buffer, length);

            return result;
        }

        std::string getExchangeAsJsonEscapedString()
        {
            std::ostringstream oss;
            std::string s = getExchangeAsString();

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
        SBE_NODISCARD std::string_view getExchangeAsStringView() const SBE_NOEXCEPT
        {
            const char *buffer = m_buffer + m_offset + 17;
            std::size_t length = 0;

            for (; length < 8 && *(buffer + length) != '\0'; ++length);
            std::string_view result(buffer, length);

            return result;
        }
        #endif

        #if __cplusplus >= 201703L
        CanonicalFilter &putExchange(const std::string_view str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putExchange [E106]");
            }

            std::memcpy(m_buffer + m_offset + 17, str.data(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 17 + start] = 0;
            }

            return *this;
        }
        #else
        CanonicalFilter &putExchange(const std::string &str)
        {
            const std::size_t srcLength = str.length();
            if (srcLength > 8)
            {
                throw std::runtime_error("string too large for putExchange [E106]");
            }

            std::memcpy(m_buffer + m_offset + 17, str.c_str(), srcLength);
            for (std::size_t start = srcLength; start < 8; ++start)
            {
                m_buffer[m_offset + 17 + start] = 0;
            }

            return *this;
        }
        #endif

        template<typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits> & operator << (
            std::basic_ostream<CharT, Traits> &builder, CanonicalFilter &writer)
        {
            builder << '{';
            builder << R"("baseCurrency": )";
            builder << '"' <<
                writer.getBaseCurrencyAsJsonEscapedString().c_str() << '"';

            builder << ", ";
            builder << R"("quoteCurrency": )";
            builder << '"' <<
                writer.getQuoteCurrencyAsJsonEscapedString().c_str() << '"';

            builder << ", ";
            builder << R"("instrumentType": )";
            builder << '"' << writer.instrumentType() << '"';

            builder << ", ";
            builder << R"("exchange": )";
            builder << '"' <<
                writer.getExchangeAsJsonEscapedString().c_str() << '"';

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
    CanonicalFilter m_canonicalFilter;

public:
    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t canonicalFilterId() SBE_NOEXCEPT
    {
        return 4;
    }

    SBE_NODISCARD inline CanonicalFilter &canonicalFilter()
    {
        m_canonicalFilter.wrapForDecode(m_buffer, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_canonicalFilter;
    }

    CanonicalFilter &canonicalFilterCount(const std::uint16_t count)
    {
        m_canonicalFilter.wrapForEncode(m_buffer, count, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_canonicalFilter;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t canonicalFilterSinceVersion() SBE_NOEXCEPT
    {
        return 0;
    }

    SBE_NODISCARD bool canonicalFilterInActingVersion() const SBE_NOEXCEPT
    {
        return true;
    }

template<typename CharT, typename Traits>
friend std::basic_ostream<CharT, Traits> & operator << (
    std::basic_ostream<CharT, Traits> &builder, const RefDataSubscriptionRequest &_writer)
{
    RefDataSubscriptionRequest writer(
        _writer.m_buffer,
        _writer.m_offset,
        _writer.m_bufferLength,
        _writer.m_actingBlockLength,
        _writer.m_actingVersion);

    builder << '{';
    builder << R"("Name": "RefDataSubscriptionRequest", )";
    builder << R"("sbeTemplateId": )";
    builder << writer.sbeTemplateId();
    builder << ", ";

    builder << R"("correlationId": )";
    builder << +writer.correlationId();

    builder << ", ";
    builder << R"("timestampNs": )";
    builder << +writer.timestampNs();

    builder << ", ";
    {
        bool atLeastOne = false;
        builder << R"("instruments": [)";
        writer.instruments().forEach(
            [&](Instruments &instruments)
            {
                if (atLeastOne)
                {
                    builder << ", ";
                }
                atLeastOne = true;
                builder << instruments;
            });
        builder << ']';
    }

    builder << ", ";
    {
        bool atLeastOne = false;
        builder << R"("canonicalFilter": [)";
        writer.canonicalFilter().forEach(
            [&](CanonicalFilter &canonicalFilter)
            {
                if (atLeastOne)
                {
                    builder << ", ";
                }
                atLeastOne = true;
                builder << canonicalFilter;
            });
        builder << ']';
    }

    builder << '}';

    return builder;
}

void skip()
{
    auto &instrumentsGroup { instruments() };
    while (instrumentsGroup.hasNext())
    {
        instrumentsGroup.next().skip();
    }
    auto &canonicalFilterGroup { canonicalFilter() };
    while (canonicalFilterGroup.hasNext())
    {
        canonicalFilterGroup.next().skip();
    }
}

SBE_NODISCARD static SBE_CONSTEXPR bool isConstLength() SBE_NOEXCEPT
{
    return false;
}

SBE_NODISCARD static std::size_t computeLength(
    std::size_t instrumentsLength = 0,
    std::size_t canonicalFilterLength = 0)
{
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
    std::size_t length = sbeBlockLength();

    length += Instruments::sbeHeaderSize();
    if (instrumentsLength > 65534LL)
    {
        throw std::runtime_error("instrumentsLength outside of allowed range [E110]");
    }
    length += instrumentsLength *Instruments::sbeBlockLength();

    length += CanonicalFilter::sbeHeaderSize();
    if (canonicalFilterLength > 65534LL)
    {
        throw std::runtime_error("canonicalFilterLength outside of allowed range [E110]");
    }
    length += canonicalFilterLength *CanonicalFilter::sbeBlockLength();

    return length;
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}
};
}
}
#endif

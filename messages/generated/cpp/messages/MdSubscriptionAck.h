/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_MDSUBSCRIPTIONACK_CXX_H_
#define _BIFROST_PROTOCOL_MDSUBSCRIPTIONACK_CXX_H_

#if __cplusplus >= 201103L
#define SBE_CONSTEXPR constexpr
#define SBE_NOEXCEPT noexcept
#else
#define SBE_CONSTEXPR
#define SBE_NOEXCEPT
#endif

#if __cplusplus >= 201703L
#include <string_view>
#define SBE_NODISCARD [[nodiscard]]
#else
#define SBE_NODISCARD
#endif

#if !defined(__STDC_LIMIT_MACROS)
#define __STDC_LIMIT_MACROS 1
#endif

#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#if defined(WIN32) || defined(_WIN32)
#define SBE_BIG_ENDIAN_ENCODE_16(v) _byteswap_ushort(v)
#define SBE_BIG_ENDIAN_ENCODE_32(v) _byteswap_ulong(v)
#define SBE_BIG_ENDIAN_ENCODE_64(v) _byteswap_uint64(v)
#define SBE_LITTLE_ENDIAN_ENCODE_16(v) (v)
#define SBE_LITTLE_ENDIAN_ENCODE_32(v) (v)
#define SBE_LITTLE_ENDIAN_ENCODE_64(v) (v)
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define SBE_BIG_ENDIAN_ENCODE_16(v) __builtin_bswap16(v)
#define SBE_BIG_ENDIAN_ENCODE_32(v) __builtin_bswap32(v)
#define SBE_BIG_ENDIAN_ENCODE_64(v) __builtin_bswap64(v)
#define SBE_LITTLE_ENDIAN_ENCODE_16(v) (v)
#define SBE_LITTLE_ENDIAN_ENCODE_32(v) (v)
#define SBE_LITTLE_ENDIAN_ENCODE_64(v) (v)
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SBE_LITTLE_ENDIAN_ENCODE_16(v) __builtin_bswap16(v)
#define SBE_LITTLE_ENDIAN_ENCODE_32(v) __builtin_bswap32(v)
#define SBE_LITTLE_ENDIAN_ENCODE_64(v) __builtin_bswap64(v)
#define SBE_BIG_ENDIAN_ENCODE_16(v) (v)
#define SBE_BIG_ENDIAN_ENCODE_32(v) (v)
#define SBE_BIG_ENDIAN_ENCODE_64(v) (v)
#else
#error "Byte Ordering of platform not determined. Set __BYTE_ORDER__ manually before including this file."
#endif

#if !defined(SBE_BOUNDS_CHECK_EXPECT)
#if defined(SBE_NO_BOUNDS_CHECK)
#define SBE_BOUNDS_CHECK_EXPECT(exp, c) (false)
#elif defined(_MSC_VER)
#define SBE_BOUNDS_CHECK_EXPECT(exp, c) (exp)
#else
#define SBE_BOUNDS_CHECK_EXPECT(exp, c) (__builtin_expect(exp, c))
#endif

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

#include "AckStatus.h"
#include "BacktestCommand.h"
#include "DeltaUpdateType.h"
#include "ExchangeId.h"
#include "ExecStatus.h"
#include "FeeCurrency.h"
#include "GroupSizeEncoding.h"
#include "InstrumentStatus.h"
#include "InstrumentType.h"
#include "MessageHeader.h"
#include "OptionSide.h"
#include "OrderSide.h"
#include "OrderType.h"
#include "RejectReason.h"
#include "RejectSource.h"
#include "RefDataErrorType.h"
#include "TimeInForce.h"
#include "TradeSide.h"

namespace bpt {
namespace messages {

class MdSubscriptionAck {
private:
    char* m_buffer = nullptr;
    std::uint64_t m_bufferLength = 0;
    std::uint64_t m_offset = 0;
    std::uint64_t m_position = 0;
    std::uint64_t m_actingBlockLength = 0;
    std::uint64_t m_actingVersion = 0;

    inline std::uint64_t* sbePositionPtr() SBE_NOEXCEPT { return &m_position; }

public:
    static const std::uint16_t SBE_BLOCK_LENGTH = static_cast<std::uint16_t>(33);
    static const std::uint16_t SBE_TEMPLATE_ID = static_cast<std::uint16_t>(5);
    static const std::uint16_t SBE_SCHEMA_ID = static_cast<std::uint16_t>(1);
    static const std::uint16_t SBE_SCHEMA_VERSION = static_cast<std::uint16_t>(12);
    static constexpr const char* SBE_SEMANTIC_VERSION = "1.12.0";

    enum MetaAttribute { EPOCH, TIME_UNIT, SEMANTIC_TYPE, PRESENCE };

    union sbe_float_as_uint_u {
        float fp_value;
        std::uint32_t uint_value;
    };

    union sbe_double_as_uint_u {
        double fp_value;
        std::uint64_t uint_value;
    };

    using messageHeader = MessageHeader;

    MdSubscriptionAck() = default;

    MdSubscriptionAck(char* buffer,
                      const std::uint64_t offset,
                      const std::uint64_t bufferLength,
                      const std::uint64_t actingBlockLength,
                      const std::uint64_t actingVersion)
        : m_buffer(buffer),
          m_bufferLength(bufferLength),
          m_offset(offset),
          m_position(sbeCheckPosition(offset + actingBlockLength)),
          m_actingBlockLength(actingBlockLength),
          m_actingVersion(actingVersion) {}

    MdSubscriptionAck(char* buffer, const std::uint64_t bufferLength)
        : MdSubscriptionAck(buffer, 0, bufferLength, sbeBlockLength(), sbeSchemaVersion()) {}

    MdSubscriptionAck(char* buffer,
                      const std::uint64_t bufferLength,
                      const std::uint64_t actingBlockLength,
                      const std::uint64_t actingVersion)
        : MdSubscriptionAck(buffer, 0, bufferLength, actingBlockLength, actingVersion) {}

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeBlockLength() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(33);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sbeBlockAndHeaderLength() SBE_NOEXCEPT {
        return messageHeader::encodedLength() + sbeBlockLength();
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeTemplateId() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(5);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeSchemaId() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(1);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeSchemaVersion() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(12);
    }

    SBE_NODISCARD static const char* sbeSemanticVersion() SBE_NOEXCEPT { return "1.12.0"; }

    SBE_NODISCARD static SBE_CONSTEXPR const char* sbeSemanticType() SBE_NOEXCEPT { return ""; }

    SBE_NODISCARD std::uint64_t offset() const SBE_NOEXCEPT { return m_offset; }

    MdSubscriptionAck& wrapForEncode(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    MdSubscriptionAck& wrapAndApplyHeader(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
        messageHeader hdr(buffer, offset, bufferLength, sbeSchemaVersion());

        hdr.blockLength(sbeBlockLength())
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

    MdSubscriptionAck& wrapForDecode(char* buffer,
                                     const std::uint64_t offset,
                                     const std::uint64_t actingBlockLength,
                                     const std::uint64_t actingVersion,
                                     const std::uint64_t bufferLength) {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = actingBlockLength;
        m_actingVersion = actingVersion;
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    MdSubscriptionAck& sbeRewind() {
        return wrapForDecode(m_buffer, m_offset, m_actingBlockLength, m_actingVersion, m_bufferLength);
    }

    SBE_NODISCARD std::uint64_t sbePosition() const SBE_NOEXCEPT { return m_position; }

    // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
    std::uint64_t sbeCheckPosition(const std::uint64_t position) {
        if (SBE_BOUNDS_CHECK_EXPECT((position > m_bufferLength), false)) {
            throw std::runtime_error("buffer too short [E100]");
        }
        return position;
    }

    void sbePosition(const std::uint64_t position) { m_position = sbeCheckPosition(position); }

    SBE_NODISCARD std::uint64_t encodedLength() const SBE_NOEXCEPT { return sbePosition() - m_offset; }

    SBE_NODISCARD std::uint64_t decodeLength() const {
        MdSubscriptionAck skipper(m_buffer, m_offset, m_bufferLength, sbeBlockLength(), m_actingVersion);
        skipper.skip();
        return skipper.encodedLength();
    }

    SBE_NODISCARD const char* buffer() const SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD char* buffer() SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD std::uint64_t bufferLength() const SBE_NOEXCEPT { return m_bufferLength; }

    SBE_NODISCARD std::uint64_t actingVersion() const SBE_NOEXCEPT { return m_actingVersion; }

    SBE_NODISCARD static const char* correlationIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t correlationIdId() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t correlationIdSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool correlationIdInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t correlationIdEncodingOffset() SBE_NOEXCEPT { return 0; }

    static SBE_CONSTEXPR std::uint64_t correlationIdNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t correlationIdMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t correlationIdMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t correlationIdEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t correlationId() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    MdSubscriptionAck& correlationId(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* timestampNsMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t timestampNsId() SBE_NOEXCEPT { return 2; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t timestampNsSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool timestampNsInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t timestampNsEncodingOffset() SBE_NOEXCEPT { return 8; }

    static SBE_CONSTEXPR std::uint64_t timestampNsNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t timestampNsMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t timestampNsMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t timestampNsEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t timestampNs() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    MdSubscriptionAck& timestampNs(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 8, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* instrumentIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t instrumentIdId() SBE_NOEXCEPT { return 3; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t instrumentIdSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool instrumentIdInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t instrumentIdEncodingOffset() SBE_NOEXCEPT { return 16; }

    static SBE_CONSTEXPR std::uint64_t instrumentIdNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t instrumentIdMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t instrumentIdMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t instrumentIdEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t instrumentId() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 16, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    MdSubscriptionAck& instrumentId(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 16, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* exchangeMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t exchangeId() SBE_NOEXCEPT { return 4; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool exchangeInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeEncodingOffset() SBE_NOEXCEPT { return 24; }

    static SBE_CONSTEXPR char exchangeNullValue() SBE_NOEXCEPT { return static_cast<char>(0); }

    static SBE_CONSTEXPR char exchangeMinValue() SBE_NOEXCEPT { return static_cast<char>(32); }

    static SBE_CONSTEXPR char exchangeMaxValue() SBE_NOEXCEPT { return static_cast<char>(126); }

    static SBE_CONSTEXPR std::size_t exchangeEncodingLength() SBE_NOEXCEPT { return 8; }

    static SBE_CONSTEXPR std::uint64_t exchangeLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD const char* exchange() const SBE_NOEXCEPT { return m_buffer + m_offset + 24; }

    SBE_NODISCARD char* exchange() SBE_NOEXCEPT { return m_buffer + m_offset + 24; }

    SBE_NODISCARD char exchange(const std::uint64_t index) const {
        if (index >= 8) {
            throw std::runtime_error("index out of range for exchange [E104]");
        }

        char val;
        std::memcpy(&val, m_buffer + m_offset + 24 + (index * 1), sizeof(char));
        return (val);
    }

    MdSubscriptionAck& exchange(const std::uint64_t index, const char value) {
        if (index >= 8) {
            throw std::runtime_error("index out of range for exchange [E105]");
        }

        char val = (value);
        std::memcpy(m_buffer + m_offset + 24 + (index * 1), &val, sizeof(char));
        return *this;
    }

    std::uint64_t getExchange(char* const dst, const std::uint64_t length) const {
        if (length > 8) {
            throw std::runtime_error("length too large for getExchange [E106]");
        }

        std::memcpy(dst, m_buffer + m_offset + 24, sizeof(char) * static_cast<std::size_t>(length));
        return length;
    }

    MdSubscriptionAck& putExchange(const char* const src) SBE_NOEXCEPT {
        std::memcpy(m_buffer + m_offset + 24, src, sizeof(char) * 8);
        return *this;
    }

    SBE_NODISCARD std::string getExchangeAsString() const {
        const char* buffer = m_buffer + m_offset + 24;
        std::size_t length = 0;

        for (; length < 8 && *(buffer + length) != '\0'; ++length)
            ;
        std::string result(buffer, length);

        return result;
    }

    std::string getExchangeAsJsonEscapedString() {
        std::ostringstream oss;
        std::string s = getExchangeAsString();

        for (const auto c : s) {
            switch (c) {
                case '"':
                    oss << "\\\"";
                    break;
                case '\\':
                    oss << "\\\\";
                    break;
                case '\b':
                    oss << "\\b";
                    break;
                case '\f':
                    oss << "\\f";
                    break;
                case '\n':
                    oss << "\\n";
                    break;
                case '\r':
                    oss << "\\r";
                    break;
                case '\t':
                    oss << "\\t";
                    break;

                default:
                    if ('\x00' <= c && c <= '\x1f') {
                        oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)(c);
                    } else {
                        oss << c;
                    }
            }
        }

        return oss.str();
    }

#if __cplusplus >= 201703L
    SBE_NODISCARD std::string_view getExchangeAsStringView() const SBE_NOEXCEPT {
        const char* buffer = m_buffer + m_offset + 24;
        std::size_t length = 0;

        for (; length < 8 && *(buffer + length) != '\0'; ++length)
            ;
        std::string_view result(buffer, length);

        return result;
    }
#endif

#if __cplusplus >= 201703L
    MdSubscriptionAck& putExchange(const std::string_view str) {
        const std::size_t srcLength = str.length();
        if (srcLength > 8) {
            throw std::runtime_error("string too large for putExchange [E106]");
        }

        std::memcpy(m_buffer + m_offset + 24, str.data(), srcLength);
        for (std::size_t start = srcLength; start < 8; ++start) {
            m_buffer[m_offset + 24 + start] = 0;
        }

        return *this;
    }
#else
    MdSubscriptionAck& putExchange(const std::string& str) {
        const std::size_t srcLength = str.length();
        if (srcLength > 8) {
            throw std::runtime_error("string too large for putExchange [E106]");
        }

        std::memcpy(m_buffer + m_offset + 24, str.c_str(), srcLength);
        for (std::size_t start = srcLength; start < 8; ++start) {
            m_buffer[m_offset + 24 + start] = 0;
        }

        return *this;
    }
#endif

    SBE_NODISCARD static const char* ackStatusMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t ackStatusId() SBE_NOEXCEPT { return 5; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t ackStatusSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool ackStatusInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t ackStatusEncodingOffset() SBE_NOEXCEPT { return 32; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t ackStatusEncodingLength() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD std::uint8_t ackStatusRaw() const SBE_NOEXCEPT {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 32, sizeof(std::uint8_t));
        return (val);
    }

    SBE_NODISCARD AckStatus::Value ackStatus() const {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 32, sizeof(std::uint8_t));
        return AckStatus::get((val));
    }

    MdSubscriptionAck& ackStatus(const AckStatus::Value value) SBE_NOEXCEPT {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 32, &val, sizeof(std::uint8_t));
        return *this;
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder,
                                                         const MdSubscriptionAck& _writer) {
        MdSubscriptionAck writer(_writer.m_buffer,
                                 _writer.m_offset,
                                 _writer.m_bufferLength,
                                 _writer.m_actingBlockLength,
                                 _writer.m_actingVersion);

        builder << '{';
        builder << R"("Name": "MdSubscriptionAck", )";
        builder << R"("sbeTemplateId": )";
        builder << writer.sbeTemplateId();
        builder << ", ";

        builder << R"("correlationId": )";
        builder << +writer.correlationId();

        builder << ", ";
        builder << R"("timestampNs": )";
        builder << +writer.timestampNs();

        builder << ", ";
        builder << R"("instrumentId": )";
        builder << +writer.instrumentId();

        builder << ", ";
        builder << R"("exchange": )";
        builder << '"' << writer.getExchangeAsJsonEscapedString().c_str() << '"';

        builder << ", ";
        builder << R"("ackStatus": )";
        builder << '"' << writer.ackStatus() << '"';

        builder << '}';

        return builder;
    }

    void skip() {}

    SBE_NODISCARD static SBE_CONSTEXPR bool isConstLength() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static std::size_t computeLength() {
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
}  // namespace protocol
}  // namespace bpt
#endif

/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_MDMARKETDATA_CXX_H_
#define _BIFROST_PROTOCOL_MDMARKETDATA_CXX_H_

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

class MdMarketData {
private:
    char* m_buffer = nullptr;
    std::uint64_t m_bufferLength = 0;
    std::uint64_t m_offset = 0;
    std::uint64_t m_position = 0;
    std::uint64_t m_actingBlockLength = 0;
    std::uint64_t m_actingVersion = 0;

    inline std::uint64_t* sbePositionPtr() SBE_NOEXCEPT { return &m_position; }

public:
    static const std::uint16_t SBE_BLOCK_LENGTH = static_cast<std::uint16_t>(56);
    static const std::uint16_t SBE_TEMPLATE_ID = static_cast<std::uint16_t>(7);
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

    MdMarketData() = default;

    MdMarketData(char* buffer,
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

    MdMarketData(char* buffer, const std::uint64_t bufferLength)
        : MdMarketData(buffer, 0, bufferLength, sbeBlockLength(), sbeSchemaVersion()) {}

    MdMarketData(char* buffer,
                 const std::uint64_t bufferLength,
                 const std::uint64_t actingBlockLength,
                 const std::uint64_t actingVersion)
        : MdMarketData(buffer, 0, bufferLength, actingBlockLength, actingVersion) {}

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeBlockLength() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(56);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sbeBlockAndHeaderLength() SBE_NOEXCEPT {
        return messageHeader::encodedLength() + sbeBlockLength();
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeTemplateId() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(7);
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

    MdMarketData& wrapForEncode(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    MdMarketData& wrapAndApplyHeader(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
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

    MdMarketData& wrapForDecode(char* buffer,
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

    MdMarketData& sbeRewind() {
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
        MdMarketData skipper(m_buffer, m_offset, m_bufferLength, sbeBlockLength(), m_actingVersion);
        skipper.skip();
        return skipper.encodedLength();
    }

    SBE_NODISCARD const char* buffer() const SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD char* buffer() SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD std::uint64_t bufferLength() const SBE_NOEXCEPT { return m_bufferLength; }

    SBE_NODISCARD std::uint64_t actingVersion() const SBE_NOEXCEPT { return m_actingVersion; }

    SBE_NODISCARD static const char* timestampNsMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t timestampNsId() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t timestampNsSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool timestampNsInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t timestampNsEncodingOffset() SBE_NOEXCEPT { return 0; }

    static SBE_CONSTEXPR std::uint64_t timestampNsNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t timestampNsMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t timestampNsMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t timestampNsEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t timestampNs() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    MdMarketData& timestampNs(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::uint64_t));
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

    static SBE_CONSTEXPR std::uint16_t instrumentIdId() SBE_NOEXCEPT { return 2; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t instrumentIdSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool instrumentIdInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t instrumentIdEncodingOffset() SBE_NOEXCEPT { return 8; }

    static SBE_CONSTEXPR std::uint64_t instrumentIdNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t instrumentIdMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t instrumentIdMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t instrumentIdEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t instrumentId() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    MdMarketData& instrumentId(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 8, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* bidPriceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t bidPriceId() SBE_NOEXCEPT { return 3; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t bidPriceSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool bidPriceInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t bidPriceEncodingOffset() SBE_NOEXCEPT { return 16; }

    static SBE_CONSTEXPR double bidPriceNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

    static SBE_CONSTEXPR double bidPriceMinValue() SBE_NOEXCEPT { return 4.9E-324; }

    static SBE_CONSTEXPR double bidPriceMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

    static SBE_CONSTEXPR std::size_t bidPriceEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD double bidPrice() const SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        std::memcpy(&val, m_buffer + m_offset + 16, sizeof(double));
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        return val.fp_value;
    }

    MdMarketData& bidPrice(const double value) SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        val.fp_value = value;
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        std::memcpy(m_buffer + m_offset + 16, &val, sizeof(double));
        return *this;
    }

    SBE_NODISCARD static const char* bidQtyMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t bidQtyId() SBE_NOEXCEPT { return 4; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t bidQtySinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool bidQtyInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t bidQtyEncodingOffset() SBE_NOEXCEPT { return 24; }

    static SBE_CONSTEXPR double bidQtyNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

    static SBE_CONSTEXPR double bidQtyMinValue() SBE_NOEXCEPT { return 4.9E-324; }

    static SBE_CONSTEXPR double bidQtyMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

    static SBE_CONSTEXPR std::size_t bidQtyEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD double bidQty() const SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        std::memcpy(&val, m_buffer + m_offset + 24, sizeof(double));
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        return val.fp_value;
    }

    MdMarketData& bidQty(const double value) SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        val.fp_value = value;
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        std::memcpy(m_buffer + m_offset + 24, &val, sizeof(double));
        return *this;
    }

    SBE_NODISCARD static const char* askPriceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t askPriceId() SBE_NOEXCEPT { return 5; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t askPriceSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool askPriceInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t askPriceEncodingOffset() SBE_NOEXCEPT { return 32; }

    static SBE_CONSTEXPR double askPriceNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

    static SBE_CONSTEXPR double askPriceMinValue() SBE_NOEXCEPT { return 4.9E-324; }

    static SBE_CONSTEXPR double askPriceMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

    static SBE_CONSTEXPR std::size_t askPriceEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD double askPrice() const SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        std::memcpy(&val, m_buffer + m_offset + 32, sizeof(double));
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        return val.fp_value;
    }

    MdMarketData& askPrice(const double value) SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        val.fp_value = value;
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        std::memcpy(m_buffer + m_offset + 32, &val, sizeof(double));
        return *this;
    }

    SBE_NODISCARD static const char* askQtyMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t askQtyId() SBE_NOEXCEPT { return 6; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t askQtySinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool askQtyInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t askQtyEncodingOffset() SBE_NOEXCEPT { return 40; }

    static SBE_CONSTEXPR double askQtyNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

    static SBE_CONSTEXPR double askQtyMinValue() SBE_NOEXCEPT { return 4.9E-324; }

    static SBE_CONSTEXPR double askQtyMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

    static SBE_CONSTEXPR std::size_t askQtyEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD double askQty() const SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        std::memcpy(&val, m_buffer + m_offset + 40, sizeof(double));
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        return val.fp_value;
    }

    MdMarketData& askQty(const double value) SBE_NOEXCEPT {
        union sbe_double_as_uint_u val;
        val.fp_value = value;
        val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
        std::memcpy(m_buffer + m_offset + 40, &val, sizeof(double));
        return *this;
    }

    SBE_NODISCARD static const char* seqNumMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t seqNumId() SBE_NOEXCEPT { return 7; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t seqNumSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool seqNumInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t seqNumEncodingOffset() SBE_NOEXCEPT { return 48; }

    static SBE_CONSTEXPR std::uint64_t seqNumNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t seqNumMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t seqNumMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t seqNumEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t seqNum() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 48, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    MdMarketData& seqNum(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 48, &val, sizeof(std::uint64_t));
        return *this;
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder,
                                                         const MdMarketData& _writer) {
        MdMarketData writer(_writer.m_buffer,
                            _writer.m_offset,
                            _writer.m_bufferLength,
                            _writer.m_actingBlockLength,
                            _writer.m_actingVersion);

        builder << '{';
        builder << R"("Name": "MdMarketData", )";
        builder << R"("sbeTemplateId": )";
        builder << writer.sbeTemplateId();
        builder << ", ";

        builder << R"("timestampNs": )";
        builder << +writer.timestampNs();

        builder << ", ";
        builder << R"("instrumentId": )";
        builder << +writer.instrumentId();

        builder << ", ";
        builder << R"("bidPrice": )";
        builder << +writer.bidPrice();

        builder << ", ";
        builder << R"("bidQty": )";
        builder << +writer.bidQty();

        builder << ", ";
        builder << R"("askPrice": )";
        builder << +writer.askPrice();

        builder << ", ";
        builder << R"("askQty": )";
        builder << +writer.askQty();

        builder << ", ";
        builder << R"("seqNum": )";
        builder << +writer.seqNum();

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

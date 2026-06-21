/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BPT_MESSAGES_NEWORDER_CXX_H_
#define _BPT_MESSAGES_NEWORDER_CXX_H_

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
#include "GroupSizeEncoding.h"
#include "InstrumentStatus.h"
#include "InstrumentType.h"
#include "MessageHeader.h"
#include "OptionSide.h"
#include "OrderSide.h"
#include "OrderType.h"
#include "RefDataErrorType.h"
#include "RejectReason.h"
#include "RejectSource.h"
#include "TimeInForce.h"
#include "TradeSide.h"

namespace bpt {
namespace messages {

class NewOrder {
private:
    char* m_buffer = nullptr;
    std::uint64_t m_bufferLength = 0;
    std::uint64_t m_offset = 0;
    std::uint64_t m_position = 0;
    std::uint64_t m_actingBlockLength = 0;
    std::uint64_t m_actingVersion = 0;

    inline std::uint64_t* sbePositionPtr() SBE_NOEXCEPT { return &m_position; }

public:
    static const std::uint16_t SBE_BLOCK_LENGTH = static_cast<std::uint16_t>(77);
    static const std::uint16_t SBE_TEMPLATE_ID = static_cast<std::uint16_t>(10);
    static const std::uint16_t SBE_SCHEMA_ID = static_cast<std::uint16_t>(1);
    static const std::uint16_t SBE_SCHEMA_VERSION = static_cast<std::uint16_t>(14);
    static constexpr const char* SBE_SEMANTIC_VERSION = "1.14.0";

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

    NewOrder() = default;

    NewOrder(char* buffer,
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

    NewOrder(char* buffer, const std::uint64_t bufferLength)
        : NewOrder(buffer, 0, bufferLength, sbeBlockLength(), sbeSchemaVersion()) {}

    NewOrder(char* buffer,
             const std::uint64_t bufferLength,
             const std::uint64_t actingBlockLength,
             const std::uint64_t actingVersion)
        : NewOrder(buffer, 0, bufferLength, actingBlockLength, actingVersion) {}

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeBlockLength() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(77);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sbeBlockAndHeaderLength() SBE_NOEXCEPT {
        return messageHeader::encodedLength() + sbeBlockLength();
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeTemplateId() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(10);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeSchemaId() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(1);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeSchemaVersion() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(14);
    }

    SBE_NODISCARD static const char* sbeSemanticVersion() SBE_NOEXCEPT { return "1.14.0"; }

    SBE_NODISCARD static SBE_CONSTEXPR const char* sbeSemanticType() SBE_NOEXCEPT { return ""; }

    SBE_NODISCARD std::uint64_t offset() const SBE_NOEXCEPT { return m_offset; }

    NewOrder& wrapForEncode(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    NewOrder& wrapAndApplyHeader(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
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

    NewOrder& wrapForDecode(char* buffer,
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

    NewOrder& sbeRewind() {
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
        NewOrder skipper(m_buffer, m_offset, m_bufferLength, sbeBlockLength(), m_actingVersion);
        skipper.skip();
        return skipper.encodedLength();
    }

    SBE_NODISCARD const char* buffer() const SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD char* buffer() SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD std::uint64_t bufferLength() const SBE_NOEXCEPT { return m_bufferLength; }

    SBE_NODISCARD std::uint64_t actingVersion() const SBE_NOEXCEPT { return m_actingVersion; }

    SBE_NODISCARD static const char* orderIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t orderIdId() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t orderIdSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool orderIdInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t orderIdEncodingOffset() SBE_NOEXCEPT { return 0; }

    static SBE_CONSTEXPR std::uint64_t orderIdNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t orderIdMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t orderIdMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t orderIdEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t orderId() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    NewOrder& orderId(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* exchangeIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t exchangeIdId() SBE_NOEXCEPT { return 2; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeIdSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool exchangeIdInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeIdEncodingOffset() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeIdEncodingLength() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD std::uint8_t exchangeIdRaw() const SBE_NOEXCEPT {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint8_t));
        return (val);
    }

    SBE_NODISCARD ExchangeId::Value exchangeId() const {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 8, sizeof(std::uint8_t));
        return ExchangeId::get((val));
    }

    NewOrder& exchangeId(const ExchangeId::Value value) SBE_NOEXCEPT {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 8, &val, sizeof(std::uint8_t));
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

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t instrumentIdEncodingOffset() SBE_NOEXCEPT { return 9; }

    static SBE_CONSTEXPR std::uint64_t instrumentIdNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t instrumentIdMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t instrumentIdMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t instrumentIdEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t instrumentId() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 9, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    NewOrder& instrumentId(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 9, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* sideMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t sideId() SBE_NOEXCEPT { return 4; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sideSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool sideInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t sideEncodingOffset() SBE_NOEXCEPT { return 17; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t sideEncodingLength() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD std::uint8_t sideRaw() const SBE_NOEXCEPT {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 17, sizeof(std::uint8_t));
        return (val);
    }

    SBE_NODISCARD OrderSide::Value side() const {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 17, sizeof(std::uint8_t));
        return OrderSide::get((val));
    }

    NewOrder& side(const OrderSide::Value value) SBE_NOEXCEPT {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 17, &val, sizeof(std::uint8_t));
        return *this;
    }

    SBE_NODISCARD static const char* orderTypeMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t orderTypeId() SBE_NOEXCEPT { return 5; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t orderTypeSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool orderTypeInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t orderTypeEncodingOffset() SBE_NOEXCEPT { return 18; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t orderTypeEncodingLength() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD std::uint8_t orderTypeRaw() const SBE_NOEXCEPT {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 18, sizeof(std::uint8_t));
        return (val);
    }

    SBE_NODISCARD OrderType::Value orderType() const {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 18, sizeof(std::uint8_t));
        return OrderType::get((val));
    }

    NewOrder& orderType(const OrderType::Value value) SBE_NOEXCEPT {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 18, &val, sizeof(std::uint8_t));
        return *this;
    }

    SBE_NODISCARD static const char* timeInForceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t timeInForceId() SBE_NOEXCEPT { return 6; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t timeInForceSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool timeInForceInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t timeInForceEncodingOffset() SBE_NOEXCEPT { return 19; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t timeInForceEncodingLength() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD std::uint8_t timeInForceRaw() const SBE_NOEXCEPT {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 19, sizeof(std::uint8_t));
        return (val);
    }

    SBE_NODISCARD TimeInForce::Value timeInForce() const {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 19, sizeof(std::uint8_t));
        return TimeInForce::get((val));
    }

    NewOrder& timeInForce(const TimeInForce::Value value) SBE_NOEXCEPT {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 19, &val, sizeof(std::uint8_t));
        return *this;
    }

    SBE_NODISCARD static const char* priceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t priceId() SBE_NOEXCEPT { return 7; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t priceSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool priceInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t priceEncodingOffset() SBE_NOEXCEPT { return 20; }

    static SBE_CONSTEXPR std::int64_t priceNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_INT64; }

    static SBE_CONSTEXPR std::int64_t priceMinValue() SBE_NOEXCEPT { return INT64_C(-9223372036854775807); }

    static SBE_CONSTEXPR std::int64_t priceMaxValue() SBE_NOEXCEPT { return INT64_C(9223372036854775807); }

    static SBE_CONSTEXPR std::size_t priceEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::int64_t price() const SBE_NOEXCEPT {
        std::int64_t val;
        std::memcpy(&val, m_buffer + m_offset + 20, sizeof(std::int64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    NewOrder& price(const std::int64_t value) SBE_NOEXCEPT {
        std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 20, &val, sizeof(std::int64_t));
        return *this;
    }

    SBE_NODISCARD static const char* quantityMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t quantityId() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t quantitySinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool quantityInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t quantityEncodingOffset() SBE_NOEXCEPT { return 28; }

    static SBE_CONSTEXPR std::uint64_t quantityNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t quantityMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t quantityMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t quantityEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t quantity() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 28, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    NewOrder& quantity(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 28, &val, sizeof(std::uint64_t));
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

    static SBE_CONSTEXPR std::uint16_t timestampNsId() SBE_NOEXCEPT { return 9; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t timestampNsSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool timestampNsInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t timestampNsEncodingOffset() SBE_NOEXCEPT { return 36; }

    static SBE_CONSTEXPR std::uint64_t timestampNsNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t timestampNsMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t timestampNsMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t timestampNsEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t timestampNs() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 36, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    NewOrder& timestampNs(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 36, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* exchangeSymbolMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t exchangeSymbolId() SBE_NOEXCEPT { return 10; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeSymbolSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool exchangeSymbolInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeSymbolEncodingOffset() SBE_NOEXCEPT { return 44; }

    static SBE_CONSTEXPR char exchangeSymbolNullValue() SBE_NOEXCEPT { return static_cast<char>(0); }

    static SBE_CONSTEXPR char exchangeSymbolMinValue() SBE_NOEXCEPT { return static_cast<char>(32); }

    static SBE_CONSTEXPR char exchangeSymbolMaxValue() SBE_NOEXCEPT { return static_cast<char>(126); }

    static SBE_CONSTEXPR std::size_t exchangeSymbolEncodingLength() SBE_NOEXCEPT { return 32; }

    static SBE_CONSTEXPR std::uint64_t exchangeSymbolLength() SBE_NOEXCEPT { return 32; }

    SBE_NODISCARD const char* exchangeSymbol() const SBE_NOEXCEPT { return m_buffer + m_offset + 44; }

    SBE_NODISCARD char* exchangeSymbol() SBE_NOEXCEPT { return m_buffer + m_offset + 44; }

    SBE_NODISCARD char exchangeSymbol(const std::uint64_t index) const {
        if (index >= 32) {
            throw std::runtime_error("index out of range for exchangeSymbol [E104]");
        }

        char val;
        std::memcpy(&val, m_buffer + m_offset + 44 + (index * 1), sizeof(char));
        return (val);
    }

    NewOrder& exchangeSymbol(const std::uint64_t index, const char value) {
        if (index >= 32) {
            throw std::runtime_error("index out of range for exchangeSymbol [E105]");
        }

        char val = (value);
        std::memcpy(m_buffer + m_offset + 44 + (index * 1), &val, sizeof(char));
        return *this;
    }

    std::uint64_t getExchangeSymbol(char* const dst, const std::uint64_t length) const {
        if (length > 32) {
            throw std::runtime_error("length too large for getExchangeSymbol [E106]");
        }

        std::memcpy(dst, m_buffer + m_offset + 44, sizeof(char) * static_cast<std::size_t>(length));
        return length;
    }

    NewOrder& putExchangeSymbol(const char* const src) SBE_NOEXCEPT {
        std::memcpy(m_buffer + m_offset + 44, src, sizeof(char) * 32);
        return *this;
    }

    SBE_NODISCARD std::string getExchangeSymbolAsString() const {
        const char* buffer = m_buffer + m_offset + 44;
        std::size_t length = 0;

        for (; length < 32 && *(buffer + length) != '\0'; ++length)
            ;
        std::string result(buffer, length);

        return result;
    }

    std::string getExchangeSymbolAsJsonEscapedString() {
        std::ostringstream oss;
        std::string s = getExchangeSymbolAsString();

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
    SBE_NODISCARD std::string_view getExchangeSymbolAsStringView() const SBE_NOEXCEPT {
        const char* buffer = m_buffer + m_offset + 44;
        std::size_t length = 0;

        for (; length < 32 && *(buffer + length) != '\0'; ++length)
            ;
        std::string_view result(buffer, length);

        return result;
    }
#endif

#if __cplusplus >= 201703L
    NewOrder& putExchangeSymbol(const std::string_view str) {
        const std::size_t srcLength = str.length();
        if (srcLength > 32) {
            throw std::runtime_error("string too large for putExchangeSymbol [E106]");
        }

        std::memcpy(m_buffer + m_offset + 44, str.data(), srcLength);
        for (std::size_t start = srcLength; start < 32; ++start) {
            m_buffer[m_offset + 44 + start] = 0;
        }

        return *this;
    }
#else
    NewOrder& putExchangeSymbol(const std::string& str) {
        const std::size_t srcLength = str.length();
        if (srcLength > 32) {
            throw std::runtime_error("string too large for putExchangeSymbol [E106]");
        }

        std::memcpy(m_buffer + m_offset + 44, str.c_str(), srcLength);
        for (std::size_t start = srcLength; start < 32; ++start) {
            m_buffer[m_offset + 44 + start] = 0;
        }

        return *this;
    }
#endif

    SBE_NODISCARD static const char* execInstMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t execInstId() SBE_NOEXCEPT { return 11; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t execInstSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool execInstInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t execInstEncodingOffset() SBE_NOEXCEPT { return 76; }

    static SBE_CONSTEXPR std::uint8_t execInstNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT8; }

    static SBE_CONSTEXPR std::uint8_t execInstMinValue() SBE_NOEXCEPT { return static_cast<std::uint8_t>(0); }

    static SBE_CONSTEXPR std::uint8_t execInstMaxValue() SBE_NOEXCEPT { return static_cast<std::uint8_t>(254); }

    static SBE_CONSTEXPR std::size_t execInstEncodingLength() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD std::uint8_t execInst() const SBE_NOEXCEPT {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 76, sizeof(std::uint8_t));
        return (val);
    }

    NewOrder& execInst(const std::uint8_t value) SBE_NOEXCEPT {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 76, &val, sizeof(std::uint8_t));
        return *this;
    }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder,
                                                         const NewOrder& _writer) {
        NewOrder writer(_writer.m_buffer,
                        _writer.m_offset,
                        _writer.m_bufferLength,
                        _writer.m_actingBlockLength,
                        _writer.m_actingVersion);

        builder << '{';
        builder << R"("Name": "NewOrder", )";
        builder << R"("sbeTemplateId": )";
        builder << writer.sbeTemplateId();
        builder << ", ";

        builder << R"("orderId": )";
        builder << +writer.orderId();

        builder << ", ";
        builder << R"("exchangeId": )";
        builder << '"' << writer.exchangeId() << '"';

        builder << ", ";
        builder << R"("instrumentId": )";
        builder << +writer.instrumentId();

        builder << ", ";
        builder << R"("side": )";
        builder << '"' << writer.side() << '"';

        builder << ", ";
        builder << R"("orderType": )";
        builder << '"' << writer.orderType() << '"';

        builder << ", ";
        builder << R"("timeInForce": )";
        builder << '"' << writer.timeInForce() << '"';

        builder << ", ";
        builder << R"("price": )";
        builder << +writer.price();

        builder << ", ";
        builder << R"("quantity": )";
        builder << +writer.quantity();

        builder << ", ";
        builder << R"("timestampNs": )";
        builder << +writer.timestampNs();

        builder << ", ";
        builder << R"("exchangeSymbol": )";
        builder << '"' << writer.getExchangeSymbolAsJsonEscapedString().c_str() << '"';

        builder << ", ";
        builder << R"("execInst": )";
        builder << +writer.execInst();

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
}  // namespace messages
}  // namespace bpt
#endif

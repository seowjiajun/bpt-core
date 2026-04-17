/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_ACCOUNTSNAPSHOT_CXX_H_
#define _BIFROST_PROTOCOL_ACCOUNTSNAPSHOT_CXX_H_

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

class AccountSnapshot {
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
    static const std::uint16_t SBE_TEMPLATE_ID = static_cast<std::uint16_t>(27);
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

    AccountSnapshot() = default;

    AccountSnapshot(char* buffer,
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

    AccountSnapshot(char* buffer, const std::uint64_t bufferLength)
        : AccountSnapshot(buffer, 0, bufferLength, sbeBlockLength(), sbeSchemaVersion()) {}

    AccountSnapshot(char* buffer,
                    const std::uint64_t bufferLength,
                    const std::uint64_t actingBlockLength,
                    const std::uint64_t actingVersion)
        : AccountSnapshot(buffer, 0, bufferLength, actingBlockLength, actingVersion) {}

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeBlockLength() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(33);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sbeBlockAndHeaderLength() SBE_NOEXCEPT {
        return messageHeader::encodedLength() + sbeBlockLength();
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeTemplateId() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(27);
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

    AccountSnapshot& wrapForEncode(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    AccountSnapshot& wrapAndApplyHeader(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
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

    AccountSnapshot& wrapForDecode(char* buffer,
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

    AccountSnapshot& sbeRewind() {
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
        AccountSnapshot skipper(m_buffer, m_offset, m_bufferLength, sbeBlockLength(), m_actingVersion);
        skipper.skip();
        return skipper.encodedLength();
    }

    SBE_NODISCARD const char* buffer() const SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD char* buffer() SBE_NOEXCEPT { return m_buffer; }

    SBE_NODISCARD std::uint64_t bufferLength() const SBE_NOEXCEPT { return m_bufferLength; }

    SBE_NODISCARD std::uint64_t actingVersion() const SBE_NOEXCEPT { return m_actingVersion; }

    SBE_NODISCARD static const char* exchangeIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t exchangeIdId() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeIdSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool exchangeIdInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeIdEncodingOffset() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeIdEncodingLength() SBE_NOEXCEPT { return 1; }

    SBE_NODISCARD std::uint8_t exchangeIdRaw() const SBE_NOEXCEPT {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint8_t));
        return (val);
    }

    SBE_NODISCARD ExchangeId::Value exchangeId() const {
        std::uint8_t val;
        std::memcpy(&val, m_buffer + m_offset + 0, sizeof(std::uint8_t));
        return ExchangeId::get((val));
    }

    AccountSnapshot& exchangeId(const ExchangeId::Value value) SBE_NOEXCEPT {
        std::uint8_t val = (value);
        std::memcpy(m_buffer + m_offset + 0, &val, sizeof(std::uint8_t));
        return *this;
    }

    SBE_NODISCARD static const char* correlationIdMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t correlationIdId() SBE_NOEXCEPT { return 2; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t correlationIdSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool correlationIdInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t correlationIdEncodingOffset() SBE_NOEXCEPT { return 1; }

    static SBE_CONSTEXPR std::uint64_t correlationIdNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t correlationIdMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t correlationIdMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t correlationIdEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t correlationId() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 1, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    AccountSnapshot& correlationId(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 1, &val, sizeof(std::uint64_t));
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

    static SBE_CONSTEXPR std::uint16_t timestampNsId() SBE_NOEXCEPT { return 3; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t timestampNsSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool timestampNsInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t timestampNsEncodingOffset() SBE_NOEXCEPT { return 9; }

    static SBE_CONSTEXPR std::uint64_t timestampNsNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t timestampNsMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t timestampNsMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t timestampNsEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t timestampNs() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 9, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    AccountSnapshot& timestampNs(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 9, &val, sizeof(std::uint64_t));
        return *this;
    }

    SBE_NODISCARD static const char* availableBalanceE8MetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t availableBalanceE8Id() SBE_NOEXCEPT { return 4; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t availableBalanceE8SinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool availableBalanceE8InActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t availableBalanceE8EncodingOffset() SBE_NOEXCEPT { return 17; }

    static SBE_CONSTEXPR std::int64_t availableBalanceE8NullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_INT64; }

    static SBE_CONSTEXPR std::int64_t availableBalanceE8MinValue() SBE_NOEXCEPT {
        return INT64_C(-9223372036854775807);
    }

    static SBE_CONSTEXPR std::int64_t availableBalanceE8MaxValue() SBE_NOEXCEPT { return INT64_C(9223372036854775807); }

    static SBE_CONSTEXPR std::size_t availableBalanceE8EncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::int64_t availableBalanceE8() const SBE_NOEXCEPT {
        std::int64_t val;
        std::memcpy(&val, m_buffer + m_offset + 17, sizeof(std::int64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    AccountSnapshot& availableBalanceE8(const std::int64_t value) SBE_NOEXCEPT {
        std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 17, &val, sizeof(std::int64_t));
        return *this;
    }

    SBE_NODISCARD static const char* totalEquityE8MetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
        switch (metaAttribute) {
            case MetaAttribute::PRESENCE:
                return "required";
            default:
                return "";
        }
    }

    static SBE_CONSTEXPR std::uint16_t totalEquityE8Id() SBE_NOEXCEPT { return 5; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t totalEquityE8SinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool totalEquityE8InActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t totalEquityE8EncodingOffset() SBE_NOEXCEPT { return 25; }

    static SBE_CONSTEXPR std::int64_t totalEquityE8NullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_INT64; }

    static SBE_CONSTEXPR std::int64_t totalEquityE8MinValue() SBE_NOEXCEPT { return INT64_C(-9223372036854775807); }

    static SBE_CONSTEXPR std::int64_t totalEquityE8MaxValue() SBE_NOEXCEPT { return INT64_C(9223372036854775807); }

    static SBE_CONSTEXPR std::size_t totalEquityE8EncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::int64_t totalEquityE8() const SBE_NOEXCEPT {
        std::int64_t val;
        std::memcpy(&val, m_buffer + m_offset + 25, sizeof(std::int64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    AccountSnapshot& totalEquityE8(const std::int64_t value) SBE_NOEXCEPT {
        std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 25, &val, sizeof(std::int64_t));
        return *this;
    }

    class Positions {
    private:
        char* m_buffer = nullptr;
        std::uint64_t m_bufferLength = 0;
        std::uint64_t m_initialPosition = 0;
        std::uint64_t* m_positionPtr = nullptr;
        std::uint64_t m_blockLength = 0;
        std::uint64_t m_count = 0;
        std::uint64_t m_index = 0;
        std::uint64_t m_offset = 0;
        std::uint64_t m_actingVersion = 0;

        SBE_NODISCARD std::uint64_t* sbePositionPtr() SBE_NOEXCEPT { return m_positionPtr; }

    public:
        Positions() = default;

        inline void wrapForDecode(char* buffer,
                                  std::uint64_t* pos,
                                  const std::uint64_t actingVersion,
                                  const std::uint64_t bufferLength) {
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

        inline void wrapForEncode(char* buffer,
                                  const std::uint16_t count,
                                  std::uint64_t* pos,
                                  const std::uint64_t actingVersion,
                                  const std::uint64_t bufferLength) {
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
            if (count > 65534) {
                throw std::runtime_error("count outside of allowed range [E110]");
            }
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
            m_buffer = buffer;
            m_bufferLength = bufferLength;
            GroupSizeEncoding dimensions(buffer, *pos, bufferLength, actingVersion);
            dimensions.blockLength(static_cast<std::uint16_t>(56));
            dimensions.numInGroup(static_cast<std::uint16_t>(count));
            m_index = 0;
            m_count = count;
            m_blockLength = 56;
            m_actingVersion = actingVersion;
            m_initialPosition = *pos;
            m_positionPtr = pos;
            *m_positionPtr = *m_positionPtr + 4;
        }

        static SBE_CONSTEXPR std::uint64_t sbeHeaderSize() SBE_NOEXCEPT { return 4; }

        static SBE_CONSTEXPR std::uint64_t sbeBlockLength() SBE_NOEXCEPT { return 56; }

        SBE_NODISCARD std::uint64_t sbePosition() const SBE_NOEXCEPT { return *m_positionPtr; }

        // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
        std::uint64_t sbeCheckPosition(const std::uint64_t position) {
            if (SBE_BOUNDS_CHECK_EXPECT((position > m_bufferLength), false)) {
                throw std::runtime_error("buffer too short [E100]");
            }
            return position;
        }

        void sbePosition(const std::uint64_t position) { *m_positionPtr = sbeCheckPosition(position); }

        SBE_NODISCARD inline std::uint64_t count() const SBE_NOEXCEPT { return m_count; }

        SBE_NODISCARD inline bool hasNext() const SBE_NOEXCEPT { return m_index < m_count; }

        inline Positions& next() {
            if (m_index >= m_count) {
                throw std::runtime_error("index >= count [E108]");
            }
            m_offset = *m_positionPtr;
            if (SBE_BOUNDS_CHECK_EXPECT(((m_offset + m_blockLength) > m_bufferLength), false)) {
                throw std::runtime_error("buffer too short for next group index [E108]");
            }
            *m_positionPtr = m_offset + m_blockLength;
            ++m_index;

            return *this;
        }

        inline std::uint64_t resetCountToIndex() {
            m_count = m_index;
            GroupSizeEncoding dimensions(m_buffer, m_initialPosition, m_bufferLength, m_actingVersion);
            dimensions.numInGroup(static_cast<std::uint16_t>(m_count));
            return m_count;
        }

        template <class Func>
        inline void forEach(Func&& func) {
            while (hasNext()) {
                next();
                func(*this);
            }
        }

        SBE_NODISCARD static const char* exchangeSymbolMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t exchangeSymbolId() SBE_NOEXCEPT { return 1; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t exchangeSymbolSinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool exchangeSymbolInActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t exchangeSymbolEncodingOffset() SBE_NOEXCEPT { return 0; }

        static SBE_CONSTEXPR char exchangeSymbolNullValue() SBE_NOEXCEPT { return static_cast<char>(0); }

        static SBE_CONSTEXPR char exchangeSymbolMinValue() SBE_NOEXCEPT { return static_cast<char>(32); }

        static SBE_CONSTEXPR char exchangeSymbolMaxValue() SBE_NOEXCEPT { return static_cast<char>(126); }

        static SBE_CONSTEXPR std::size_t exchangeSymbolEncodingLength() SBE_NOEXCEPT { return 32; }

        static SBE_CONSTEXPR std::uint64_t exchangeSymbolLength() SBE_NOEXCEPT { return 32; }

        SBE_NODISCARD const char* exchangeSymbol() const SBE_NOEXCEPT { return m_buffer + m_offset + 0; }

        SBE_NODISCARD char* exchangeSymbol() SBE_NOEXCEPT { return m_buffer + m_offset + 0; }

        SBE_NODISCARD char exchangeSymbol(const std::uint64_t index) const {
            if (index >= 32) {
                throw std::runtime_error("index out of range for exchangeSymbol [E104]");
            }

            char val;
            std::memcpy(&val, m_buffer + m_offset + 0 + (index * 1), sizeof(char));
            return (val);
        }

        Positions& exchangeSymbol(const std::uint64_t index, const char value) {
            if (index >= 32) {
                throw std::runtime_error("index out of range for exchangeSymbol [E105]");
            }

            char val = (value);
            std::memcpy(m_buffer + m_offset + 0 + (index * 1), &val, sizeof(char));
            return *this;
        }

        std::uint64_t getExchangeSymbol(char* const dst, const std::uint64_t length) const {
            if (length > 32) {
                throw std::runtime_error("length too large for getExchangeSymbol [E106]");
            }

            std::memcpy(dst, m_buffer + m_offset + 0, sizeof(char) * static_cast<std::size_t>(length));
            return length;
        }

        Positions& putExchangeSymbol(const char* const src) SBE_NOEXCEPT {
            std::memcpy(m_buffer + m_offset + 0, src, sizeof(char) * 32);
            return *this;
        }

        SBE_NODISCARD std::string getExchangeSymbolAsString() const {
            const char* buffer = m_buffer + m_offset + 0;
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
            const char* buffer = m_buffer + m_offset + 0;
            std::size_t length = 0;

            for (; length < 32 && *(buffer + length) != '\0'; ++length)
                ;
            std::string_view result(buffer, length);

            return result;
        }
#endif

#if __cplusplus >= 201703L
        Positions& putExchangeSymbol(const std::string_view str) {
            const std::size_t srcLength = str.length();
            if (srcLength > 32) {
                throw std::runtime_error("string too large for putExchangeSymbol [E106]");
            }

            std::memcpy(m_buffer + m_offset + 0, str.data(), srcLength);
            for (std::size_t start = srcLength; start < 32; ++start) {
                m_buffer[m_offset + 0 + start] = 0;
            }

            return *this;
        }
#else
        Positions& putExchangeSymbol(const std::string& str) {
            const std::size_t srcLength = str.length();
            if (srcLength > 32) {
                throw std::runtime_error("string too large for putExchangeSymbol [E106]");
            }

            std::memcpy(m_buffer + m_offset + 0, str.c_str(), srcLength);
            for (std::size_t start = srcLength; start < 32; ++start) {
                m_buffer[m_offset + 0 + start] = 0;
            }

            return *this;
        }
#endif

        SBE_NODISCARD static const char* netQtyE8MetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t netQtyE8Id() SBE_NOEXCEPT { return 2; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t netQtyE8SinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool netQtyE8InActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t netQtyE8EncodingOffset() SBE_NOEXCEPT { return 32; }

        static SBE_CONSTEXPR std::int64_t netQtyE8NullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_INT64; }

        static SBE_CONSTEXPR std::int64_t netQtyE8MinValue() SBE_NOEXCEPT { return INT64_C(-9223372036854775807); }

        static SBE_CONSTEXPR std::int64_t netQtyE8MaxValue() SBE_NOEXCEPT { return INT64_C(9223372036854775807); }

        static SBE_CONSTEXPR std::size_t netQtyE8EncodingLength() SBE_NOEXCEPT { return 8; }

        SBE_NODISCARD std::int64_t netQtyE8() const SBE_NOEXCEPT {
            std::int64_t val;
            std::memcpy(&val, m_buffer + m_offset + 32, sizeof(std::int64_t));
            return SBE_LITTLE_ENDIAN_ENCODE_64(val);
        }

        Positions& netQtyE8(const std::int64_t value) SBE_NOEXCEPT {
            std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
            std::memcpy(m_buffer + m_offset + 32, &val, sizeof(std::int64_t));
            return *this;
        }

        SBE_NODISCARD static const char* avgEntryPriceE8MetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t avgEntryPriceE8Id() SBE_NOEXCEPT { return 3; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t avgEntryPriceE8SinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool avgEntryPriceE8InActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t avgEntryPriceE8EncodingOffset() SBE_NOEXCEPT { return 40; }

        static SBE_CONSTEXPR std::int64_t avgEntryPriceE8NullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_INT64; }

        static SBE_CONSTEXPR std::int64_t avgEntryPriceE8MinValue() SBE_NOEXCEPT {
            return INT64_C(-9223372036854775807);
        }

        static SBE_CONSTEXPR std::int64_t avgEntryPriceE8MaxValue() SBE_NOEXCEPT {
            return INT64_C(9223372036854775807);
        }

        static SBE_CONSTEXPR std::size_t avgEntryPriceE8EncodingLength() SBE_NOEXCEPT { return 8; }

        SBE_NODISCARD std::int64_t avgEntryPriceE8() const SBE_NOEXCEPT {
            std::int64_t val;
            std::memcpy(&val, m_buffer + m_offset + 40, sizeof(std::int64_t));
            return SBE_LITTLE_ENDIAN_ENCODE_64(val);
        }

        Positions& avgEntryPriceE8(const std::int64_t value) SBE_NOEXCEPT {
            std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
            std::memcpy(m_buffer + m_offset + 40, &val, sizeof(std::int64_t));
            return *this;
        }

        SBE_NODISCARD static const char* unrealizedPnlE8MetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t unrealizedPnlE8Id() SBE_NOEXCEPT { return 4; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t unrealizedPnlE8SinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool unrealizedPnlE8InActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t unrealizedPnlE8EncodingOffset() SBE_NOEXCEPT { return 48; }

        static SBE_CONSTEXPR std::int64_t unrealizedPnlE8NullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_INT64; }

        static SBE_CONSTEXPR std::int64_t unrealizedPnlE8MinValue() SBE_NOEXCEPT {
            return INT64_C(-9223372036854775807);
        }

        static SBE_CONSTEXPR std::int64_t unrealizedPnlE8MaxValue() SBE_NOEXCEPT {
            return INT64_C(9223372036854775807);
        }

        static SBE_CONSTEXPR std::size_t unrealizedPnlE8EncodingLength() SBE_NOEXCEPT { return 8; }

        SBE_NODISCARD std::int64_t unrealizedPnlE8() const SBE_NOEXCEPT {
            std::int64_t val;
            std::memcpy(&val, m_buffer + m_offset + 48, sizeof(std::int64_t));
            return SBE_LITTLE_ENDIAN_ENCODE_64(val);
        }

        Positions& unrealizedPnlE8(const std::int64_t value) SBE_NOEXCEPT {
            std::int64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
            std::memcpy(m_buffer + m_offset + 48, &val, sizeof(std::int64_t));
            return *this;
        }

        template <typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder,
                                                             Positions& writer) {
            builder << '{';
            builder << R"("exchangeSymbol": )";
            builder << '"' << writer.getExchangeSymbolAsJsonEscapedString().c_str() << '"';

            builder << ", ";
            builder << R"("netQtyE8": )";
            builder << +writer.netQtyE8();

            builder << ", ";
            builder << R"("avgEntryPriceE8": )";
            builder << +writer.avgEntryPriceE8();

            builder << ", ";
            builder << R"("unrealizedPnlE8": )";
            builder << +writer.unrealizedPnlE8();

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

private:
    Positions m_positions;

public:
    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t positionsId() SBE_NOEXCEPT { return 6; }

    SBE_NODISCARD inline Positions& positions() {
        m_positions.wrapForDecode(m_buffer, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_positions;
    }

    Positions& positionsCount(const std::uint16_t count) {
        m_positions.wrapForEncode(m_buffer, count, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_positions;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t positionsSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool positionsInActingVersion() const SBE_NOEXCEPT { return true; }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder,
                                                         const AccountSnapshot& _writer) {
        AccountSnapshot writer(_writer.m_buffer,
                               _writer.m_offset,
                               _writer.m_bufferLength,
                               _writer.m_actingBlockLength,
                               _writer.m_actingVersion);

        builder << '{';
        builder << R"("Name": "AccountSnapshot", )";
        builder << R"("sbeTemplateId": )";
        builder << writer.sbeTemplateId();
        builder << ", ";

        builder << R"("exchangeId": )";
        builder << '"' << writer.exchangeId() << '"';

        builder << ", ";
        builder << R"("correlationId": )";
        builder << +writer.correlationId();

        builder << ", ";
        builder << R"("timestampNs": )";
        builder << +writer.timestampNs();

        builder << ", ";
        builder << R"("availableBalanceE8": )";
        builder << +writer.availableBalanceE8();

        builder << ", ";
        builder << R"("totalEquityE8": )";
        builder << +writer.totalEquityE8();

        builder << ", ";
        {
            bool atLeastOne = false;
            builder << R"("positions": [)";
            writer.positions().forEach([&](Positions& positions) {
                if (atLeastOne) {
                    builder << ", ";
                }
                atLeastOne = true;
                builder << positions;
            });
            builder << ']';
        }

        builder << '}';

        return builder;
    }

    void skip() {
        auto& positionsGroup{positions()};
        while (positionsGroup.hasNext()) {
            positionsGroup.next().skip();
        }
    }

    SBE_NODISCARD static SBE_CONSTEXPR bool isConstLength() SBE_NOEXCEPT { return false; }

    SBE_NODISCARD static std::size_t computeLength(std::size_t positionsLength = 0) {
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
        std::size_t length = sbeBlockLength();

        length += Positions::sbeHeaderSize();
        if (positionsLength > 65534LL) {
            throw std::runtime_error("positionsLength outside of allowed range [E110]");
        }
        length += positionsLength * Positions::sbeBlockLength();

        return length;
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }
};
}  // namespace protocol
}  // namespace bpt
#endif

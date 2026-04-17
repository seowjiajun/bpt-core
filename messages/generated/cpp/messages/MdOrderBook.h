/* Generated SBE (Simple Binary Encoding) message codec */
#ifndef _BIFROST_PROTOCOL_MDORDERBOOK_CXX_H_
#define _BIFROST_PROTOCOL_MDORDERBOOK_CXX_H_

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

class MdOrderBook {
private:
    char* m_buffer = nullptr;
    std::uint64_t m_bufferLength = 0;
    std::uint64_t m_offset = 0;
    std::uint64_t m_position = 0;
    std::uint64_t m_actingBlockLength = 0;
    std::uint64_t m_actingVersion = 0;

    inline std::uint64_t* sbePositionPtr() SBE_NOEXCEPT { return &m_position; }

public:
    static const std::uint16_t SBE_BLOCK_LENGTH = static_cast<std::uint16_t>(24);
    static const std::uint16_t SBE_TEMPLATE_ID = static_cast<std::uint16_t>(20);
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

    MdOrderBook() = default;

    MdOrderBook(char* buffer,
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

    MdOrderBook(char* buffer, const std::uint64_t bufferLength)
        : MdOrderBook(buffer, 0, bufferLength, sbeBlockLength(), sbeSchemaVersion()) {}

    MdOrderBook(char* buffer,
                const std::uint64_t bufferLength,
                const std::uint64_t actingBlockLength,
                const std::uint64_t actingVersion)
        : MdOrderBook(buffer, 0, bufferLength, actingBlockLength, actingVersion) {}

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeBlockLength() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(24);
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t sbeBlockAndHeaderLength() SBE_NOEXCEPT {
        return messageHeader::encodedLength() + sbeBlockLength();
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t sbeTemplateId() SBE_NOEXCEPT {
        return static_cast<std::uint16_t>(20);
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

    MdOrderBook& wrapForEncode(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
        m_buffer = buffer;
        m_bufferLength = bufferLength;
        m_offset = offset;
        m_actingBlockLength = sbeBlockLength();
        m_actingVersion = sbeSchemaVersion();
        m_position = sbeCheckPosition(m_offset + m_actingBlockLength);
        return *this;
    }

    MdOrderBook& wrapAndApplyHeader(char* buffer, const std::uint64_t offset, const std::uint64_t bufferLength) {
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

    MdOrderBook& wrapForDecode(char* buffer,
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

    MdOrderBook& sbeRewind() {
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
        MdOrderBook skipper(m_buffer, m_offset, m_bufferLength, sbeBlockLength(), m_actingVersion);
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

    MdOrderBook& timestampNs(const std::uint64_t value) SBE_NOEXCEPT {
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

    MdOrderBook& instrumentId(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 8, &val, sizeof(std::uint64_t));
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

    static SBE_CONSTEXPR std::uint16_t seqNumId() SBE_NOEXCEPT { return 3; }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t seqNumSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool seqNumInActingVersion() SBE_NOEXCEPT { return true; }

    SBE_NODISCARD static SBE_CONSTEXPR std::size_t seqNumEncodingOffset() SBE_NOEXCEPT { return 16; }

    static SBE_CONSTEXPR std::uint64_t seqNumNullValue() SBE_NOEXCEPT { return SBE_NULLVALUE_UINT64; }

    static SBE_CONSTEXPR std::uint64_t seqNumMinValue() SBE_NOEXCEPT { return UINT64_C(0x0); }

    static SBE_CONSTEXPR std::uint64_t seqNumMaxValue() SBE_NOEXCEPT { return UINT64_C(0xfffffffffffffffe); }

    static SBE_CONSTEXPR std::size_t seqNumEncodingLength() SBE_NOEXCEPT { return 8; }

    SBE_NODISCARD std::uint64_t seqNum() const SBE_NOEXCEPT {
        std::uint64_t val;
        std::memcpy(&val, m_buffer + m_offset + 16, sizeof(std::uint64_t));
        return SBE_LITTLE_ENDIAN_ENCODE_64(val);
    }

    MdOrderBook& seqNum(const std::uint64_t value) SBE_NOEXCEPT {
        std::uint64_t val = SBE_LITTLE_ENDIAN_ENCODE_64(value);
        std::memcpy(m_buffer + m_offset + 16, &val, sizeof(std::uint64_t));
        return *this;
    }

    class Bids {
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
        Bids() = default;

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
            dimensions.blockLength(static_cast<std::uint16_t>(16));
            dimensions.numInGroup(static_cast<std::uint16_t>(count));
            m_index = 0;
            m_count = count;
            m_blockLength = 16;
            m_actingVersion = actingVersion;
            m_initialPosition = *pos;
            m_positionPtr = pos;
            *m_positionPtr = *m_positionPtr + 4;
        }

        static SBE_CONSTEXPR std::uint64_t sbeHeaderSize() SBE_NOEXCEPT { return 4; }

        static SBE_CONSTEXPR std::uint64_t sbeBlockLength() SBE_NOEXCEPT { return 16; }

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

        inline Bids& next() {
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

        SBE_NODISCARD static const char* priceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t priceId() SBE_NOEXCEPT { return 1; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t priceSinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool priceInActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t priceEncodingOffset() SBE_NOEXCEPT { return 0; }

        static SBE_CONSTEXPR double priceNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

        static SBE_CONSTEXPR double priceMinValue() SBE_NOEXCEPT { return 4.9E-324; }

        static SBE_CONSTEXPR double priceMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

        static SBE_CONSTEXPR std::size_t priceEncodingLength() SBE_NOEXCEPT { return 8; }

        SBE_NODISCARD double price() const SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 0, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Bids& price(const double value) SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 0, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char* qtyMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t qtyId() SBE_NOEXCEPT { return 2; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t qtySinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool qtyInActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t qtyEncodingOffset() SBE_NOEXCEPT { return 8; }

        static SBE_CONSTEXPR double qtyNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

        static SBE_CONSTEXPR double qtyMinValue() SBE_NOEXCEPT { return 4.9E-324; }

        static SBE_CONSTEXPR double qtyMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

        static SBE_CONSTEXPR std::size_t qtyEncodingLength() SBE_NOEXCEPT { return 8; }

        SBE_NODISCARD double qty() const SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 8, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Bids& qty(const double value) SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 8, &val, sizeof(double));
            return *this;
        }

        template <typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder, Bids& writer) {
            builder << '{';
            builder << R"("price": )";
            builder << +writer.price();

            builder << ", ";
            builder << R"("qty": )";
            builder << +writer.qty();

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
    Bids m_bids;

public:
    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t bidsId() SBE_NOEXCEPT { return 4; }

    SBE_NODISCARD inline Bids& bids() {
        m_bids.wrapForDecode(m_buffer, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_bids;
    }

    Bids& bidsCount(const std::uint16_t count) {
        m_bids.wrapForEncode(m_buffer, count, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_bids;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t bidsSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool bidsInActingVersion() const SBE_NOEXCEPT { return true; }

    class Asks {
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
        Asks() = default;

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
            dimensions.blockLength(static_cast<std::uint16_t>(16));
            dimensions.numInGroup(static_cast<std::uint16_t>(count));
            m_index = 0;
            m_count = count;
            m_blockLength = 16;
            m_actingVersion = actingVersion;
            m_initialPosition = *pos;
            m_positionPtr = pos;
            *m_positionPtr = *m_positionPtr + 4;
        }

        static SBE_CONSTEXPR std::uint64_t sbeHeaderSize() SBE_NOEXCEPT { return 4; }

        static SBE_CONSTEXPR std::uint64_t sbeBlockLength() SBE_NOEXCEPT { return 16; }

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

        inline Asks& next() {
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

        SBE_NODISCARD static const char* priceMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t priceId() SBE_NOEXCEPT { return 1; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t priceSinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool priceInActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t priceEncodingOffset() SBE_NOEXCEPT { return 0; }

        static SBE_CONSTEXPR double priceNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

        static SBE_CONSTEXPR double priceMinValue() SBE_NOEXCEPT { return 4.9E-324; }

        static SBE_CONSTEXPR double priceMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

        static SBE_CONSTEXPR std::size_t priceEncodingLength() SBE_NOEXCEPT { return 8; }

        SBE_NODISCARD double price() const SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 0, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Asks& price(const double value) SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 0, &val, sizeof(double));
            return *this;
        }

        SBE_NODISCARD static const char* qtyMetaAttribute(const MetaAttribute metaAttribute) SBE_NOEXCEPT {
            switch (metaAttribute) {
                case MetaAttribute::PRESENCE:
                    return "required";
                default:
                    return "";
            }
        }

        static SBE_CONSTEXPR std::uint16_t qtyId() SBE_NOEXCEPT { return 2; }

        SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t qtySinceVersion() SBE_NOEXCEPT { return 0; }

        SBE_NODISCARD bool qtyInActingVersion() SBE_NOEXCEPT { return true; }

        SBE_NODISCARD static SBE_CONSTEXPR std::size_t qtyEncodingOffset() SBE_NOEXCEPT { return 8; }

        static SBE_CONSTEXPR double qtyNullValue() SBE_NOEXCEPT { return SBE_DOUBLE_NAN; }

        static SBE_CONSTEXPR double qtyMinValue() SBE_NOEXCEPT { return 4.9E-324; }

        static SBE_CONSTEXPR double qtyMaxValue() SBE_NOEXCEPT { return 1.7976931348623157E308; }

        static SBE_CONSTEXPR std::size_t qtyEncodingLength() SBE_NOEXCEPT { return 8; }

        SBE_NODISCARD double qty() const SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            std::memcpy(&val, m_buffer + m_offset + 8, sizeof(double));
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            return val.fp_value;
        }

        Asks& qty(const double value) SBE_NOEXCEPT {
            union sbe_double_as_uint_u val;
            val.fp_value = value;
            val.uint_value = SBE_LITTLE_ENDIAN_ENCODE_64(val.uint_value);
            std::memcpy(m_buffer + m_offset + 8, &val, sizeof(double));
            return *this;
        }

        template <typename CharT, typename Traits>
        friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder, Asks& writer) {
            builder << '{';
            builder << R"("price": )";
            builder << +writer.price();

            builder << ", ";
            builder << R"("qty": )";
            builder << +writer.qty();

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
    Asks m_asks;

public:
    SBE_NODISCARD static SBE_CONSTEXPR std::uint16_t asksId() SBE_NOEXCEPT { return 5; }

    SBE_NODISCARD inline Asks& asks() {
        m_asks.wrapForDecode(m_buffer, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_asks;
    }

    Asks& asksCount(const std::uint16_t count) {
        m_asks.wrapForEncode(m_buffer, count, sbePositionPtr(), m_actingVersion, m_bufferLength);
        return m_asks;
    }

    SBE_NODISCARD static SBE_CONSTEXPR std::uint64_t asksSinceVersion() SBE_NOEXCEPT { return 0; }

    SBE_NODISCARD bool asksInActingVersion() const SBE_NOEXCEPT { return true; }

    template <typename CharT, typename Traits>
    friend std::basic_ostream<CharT, Traits>& operator<<(std::basic_ostream<CharT, Traits>& builder,
                                                         const MdOrderBook& _writer) {
        MdOrderBook writer(_writer.m_buffer,
                           _writer.m_offset,
                           _writer.m_bufferLength,
                           _writer.m_actingBlockLength,
                           _writer.m_actingVersion);

        builder << '{';
        builder << R"("Name": "MdOrderBook", )";
        builder << R"("sbeTemplateId": )";
        builder << writer.sbeTemplateId();
        builder << ", ";

        builder << R"("timestampNs": )";
        builder << +writer.timestampNs();

        builder << ", ";
        builder << R"("instrumentId": )";
        builder << +writer.instrumentId();

        builder << ", ";
        builder << R"("seqNum": )";
        builder << +writer.seqNum();

        builder << ", ";
        {
            bool atLeastOne = false;
            builder << R"("bids": [)";
            writer.bids().forEach([&](Bids& bids) {
                if (atLeastOne) {
                    builder << ", ";
                }
                atLeastOne = true;
                builder << bids;
            });
            builder << ']';
        }

        builder << ", ";
        {
            bool atLeastOne = false;
            builder << R"("asks": [)";
            writer.asks().forEach([&](Asks& asks) {
                if (atLeastOne) {
                    builder << ", ";
                }
                atLeastOne = true;
                builder << asks;
            });
            builder << ']';
        }

        builder << '}';

        return builder;
    }

    void skip() {
        auto& bidsGroup{bids()};
        while (bidsGroup.hasNext()) {
            bidsGroup.next().skip();
        }
        auto& asksGroup{asks()};
        while (asksGroup.hasNext()) {
            asksGroup.next().skip();
        }
    }

    SBE_NODISCARD static SBE_CONSTEXPR bool isConstLength() SBE_NOEXCEPT { return false; }

    SBE_NODISCARD static std::size_t computeLength(std::size_t bidsLength = 0, std::size_t asksLength = 0) {
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
        std::size_t length = sbeBlockLength();

        length += Bids::sbeHeaderSize();
        if (bidsLength > 65534LL) {
            throw std::runtime_error("bidsLength outside of allowed range [E110]");
        }
        length += bidsLength * Bids::sbeBlockLength();

        length += Asks::sbeHeaderSize();
        if (asksLength > 65534LL) {
            throw std::runtime_error("asksLength outside of allowed range [E110]");
        }
        length += asksLength * Asks::sbeBlockLength();

        return length;
#if defined(__GNUG__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    }
};
}  // namespace protocol
}  // namespace bpt
#endif

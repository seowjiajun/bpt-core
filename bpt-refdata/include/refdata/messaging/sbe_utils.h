#pragma once

#include <Aeron.h>

#include <messages/InstrumentStatus.h>
#include <messages/InstrumentType.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <cstring>
#include <ctime>
#include <refdata/refdata/instrument.h>
#include <x86intrin.h>

namespace bpt::refdata::messaging {

// Max _mm_pause() spins before giving up on Aeron backpressure.
// ~40ns per pause on modern CPUs → 25 000 spins ≈ 1 ms.
constexpr int kAeronMaxSpins = 25000;

// Offer a buffer to an Aeron publication with bounded spin on backpressure.
// Breaks immediately on NOT_CONNECTED / PUBLICATION_CLOSED.
// Logs a warning and returns if the spin limit is exceeded (subscriber too slow or dead).
inline void aeron_offer(aeron::Publication& pub,
                        const aeron::AtomicBuffer& buf,
                        aeron::util::index_t len,
                        const char* context = "") {
    int spins = 0;
    std::int64_t r;
    do {
        r = pub.offer(buf, 0, len);
        if (r == aeron::NOT_CONNECTED || r == aeron::PUBLICATION_CLOSED)
            return;
        if (r < 0) {
            _mm_pause();
            if (++spins == kAeronMaxSpins) {
                bpt::common::log::warn("[Aeron] Backpressure spin limit reached — dropping {}", context);
                return;
            }
        }
    } while (r < 0);
}

inline bpt::messages::InstrumentType::Value to_sbe_type(refdata::InstrumentType t) {
    using S = refdata::InstrumentType;
    using P = bpt::messages::InstrumentType;
    switch (t) {
        case S::SPOT:
            return P::SPOT;
        case S::FUTURE:
            return P::FUTURE;
        case S::PERP:
            return P::PERPETUAL;
        case S::OPTION:
            return P::OPTION;
        default:
            return P::NULL_VALUE;
    }
}

inline bpt::messages::InstrumentStatus::Value to_sbe_status(refdata::InstrumentStatus s) {
    using S = refdata::InstrumentStatus;
    using P = bpt::messages::InstrumentStatus;
    switch (s) {
        case S::ACTIVE:
            return P::ACTIVE;
        case S::HALTED:
            return P::HALTED;
        case S::DELISTED:
            return P::INACTIVE;
        default:
            return P::NULL_VALUE;
    }
}

inline uint32_t ns_to_yyyymmdd(uint64_t ns) {
    time_t t = static_cast<time_t>(ns / 1'000'000'000ULL);
    struct tm tm_info{};
    gmtime_r(&t, &tm_info);
    return static_cast<uint32_t>((tm_info.tm_year + 1900) * 10000 + (tm_info.tm_mon + 1) * 100 + tm_info.tm_mday);
}

template <std::size_t N>
void put_str(char* dst, std::string_view src) {
    std::size_t len = std::min(src.size(), N);
    std::memcpy(dst, src.data(), len);
    if (len < N)
        std::memset(dst + len, 0, N - len);
}

}  // namespace bpt::refdata::messaging

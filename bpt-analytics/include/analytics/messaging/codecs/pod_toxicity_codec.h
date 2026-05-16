#pragma once

/// @file
/// POD memcpy codec for the analytics ToxicityUpdate stream.
///
/// Toxicity is not SBE-encoded — it ships the POD struct as raw bytes
/// on Aeron, with the consumer doing a memcpy back to the struct. This
/// codec wraps that as a `Codec<C, T>`-conforming utility so the
/// pattern is uniform across services: every publisher composes a
/// codec, even when the codec is a trivial memcpy.
///
/// Demonstrates that the encoder/decoder abstraction works for any
/// wire format, not just SBE.

#include "analytics/messaging/toxicity_update.h"
#include "bpt_common/codec/codec.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <stdexcept>

namespace bpt::analytics::messaging {

class PodToxicityCodec {
public:
    std::span<const std::byte> encode(const ToxicityUpdate& u, std::span<std::byte> scratch) {
        if (scratch.size() < sizeof(ToxicityUpdate))
            throw std::runtime_error("PodToxicityCodec::encode: scratch too small");
        std::memcpy(scratch.data(), &u, sizeof(ToxicityUpdate));
        return scratch.subspan(0, sizeof(ToxicityUpdate));
    }

    ToxicityUpdate decode(std::span<const std::byte> bytes) {
        if (bytes.size() != sizeof(ToxicityUpdate))
            throw std::runtime_error("PodToxicityCodec::decode: wrong size");
        ToxicityUpdate u;
        std::memcpy(&u, bytes.data(), sizeof(ToxicityUpdate));
        return u;
    }

    static constexpr std::size_t kRecommendedScratchSize = sizeof(ToxicityUpdate);
};

static_assert(bpt::common::codec::Codec<PodToxicityCodec, ToxicityUpdate>);

}  // namespace bpt::analytics::messaging

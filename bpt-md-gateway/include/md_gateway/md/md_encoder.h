#pragma once

#include "md_gateway/md/md_types.h"

#include <messages/MdOrderBook.h>
#include <messages/MessageHeader.h>

#include <cstddef>

namespace bpt::md_gateway::md {

// Stateless SBE encoder for variable-length MD types.
//
// BBO and Trade are encoded inline in MdPublisher via the zero-copy
// publish<T> path; only OrderBook still goes through this helper because
// its payload size is dynamic and can't use tryClaim.
class MdEncoder {
public:
    // --- OrderBook ----------------------------------------------------------
    // Max supported levels per side. Books larger than this are silently dropped.
    // Aliased to the domain-side constant in md_types.h so the InlineVec
    // capacity and the encoder's per-side cap can never drift apart.
    static constexpr std::size_t kMaxLevels = kMaxBookLevels;
    // kMaxLevels * 2 sides * 16 bytes/level + headers fits well under 2 KiB.
    static constexpr std::size_t kMaxOrderBookBufSize = 2048;

    // Returns number of bytes written, or 0 if the book exceeds kMaxLevels.
    static std::size_t encode(const MdOrderBook& book, uint64_t seq_num, char* buf, std::size_t capacity) noexcept;
};

}  // namespace bpt::md_gateway::md

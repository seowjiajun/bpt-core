#pragma once

#include "md_gateway/md/md_encoder.h"
#include "md_gateway/md/md_types.h"

#include <Aeron.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::md_gateway::messaging {

/// Publishes normalised market-data structs on stream 2002 (MdGateway→Strategy).
///
/// BBO and Trade use the zero-copy publish<T> path — SBE encodes directly into
/// the Aeron log buffer via tryClaim. OrderBook stays on offer() because its
/// payload is variable-length (up to kMaxLevels per side).
///
/// Thread-safe: multiple adapter threads may call publish() concurrently.
/// Publisher's internal mutex serialises tryClaim/offer; seq_ uses relaxed
/// fetch_add — each message carries its own sequence number.
///
/// No virtual interface — venue decoders are templated on the inner pub
/// type so the publish() chain inlines all the way down.
class MdPublisher {
public:
    MdPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const md::MdBbo& bbo);
    void publish(const md::MdTrade& trade);
    void publish(const md::MdOrderBook& book);

    [[nodiscard]] uint64_t current_seq() const { return seq_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t drop_count() const { return drops_.load(std::memory_order_relaxed); }

private:
    void record_drop(uint64_t instrument_id, const char* label);

    bpt::common::aeron::Publisher publisher_;
    /// Cache-line isolated: the publisher thread does a `lock add` on seq_
    /// every publish; MdGatewayApp's reporter reads drop_count() every
    /// ~10µs idle iteration. Separating them keeps the reporter's read
    /// out of the publisher's hot line.
    alignas(64) std::atomic<uint64_t> seq_{0};
    alignas(64) std::atomic<uint64_t> drops_{0};
};

}  // namespace bpt::md_gateway::messaging

#pragma once

/// \file
/// \brief Concrete MD publisher — encodes and publishes BBO / Trade /
///        OrderBook on stream 2002 (MdGateway → Strategy).
///
/// No virtual interface: venue decoders are templated on the inner
/// publisher type (CRTP), so the decoder → ValidatingPublisher →
/// MdPublisher chain inlines all the way down. The optimizer can fold
/// the SBE encode block, the seq increment, and the publication offer
/// into a single straight-line code path on the hot tick path.

#include "md_gateway/md/md_encoder.h"
#include "md_gateway/md/md_types.h"

#include <Aeron.h>

#include <atomic>
#include <bpt_common/aeron/publisher.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::md_gateway::messaging {

/// \brief Publishes normalised market-data structs on the MdGateway→Strategy stream.
///
/// BBO and Trade use the zero-copy publish<T> path — SBE encodes
/// directly into the Aeron log buffer via tryClaim. OrderBook stays on
/// offer() because its payload is variable-length (up to kMaxLevels
/// per side).
///
/// Thread-safe: multiple adapter threads may call publish()
/// concurrently. The wrapped Publisher's internal mutex serialises
/// tryClaim/offer; seq_ uses relaxed fetch_add so each published
/// message carries its own sequence number without inter-thread
/// ordering cost beyond what Aeron itself requires.
class MdPublisher {
public:
    /// \brief Construct with a live Aeron client and the destination stream.
    ///
    /// \param aeron      shared Aeron client (typically the bus-built singleton)
    /// \param channel    Aeron URI (`aeron:ipc` or `aeron:udp?endpoint=...`)
    /// \param stream_id  publication stream — 2002 in the production topology
    MdPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// \brief Publish a top-of-book quote. Zero-copy via tryClaim.
    void publish(const md::MdBbo& bbo);

    /// \brief Publish a trade print. Zero-copy via tryClaim.
    void publish(const md::MdTrade& trade);

    /// \brief Publish an order-book snapshot.
    ///
    /// Uses offer() rather than tryClaim because the encoded payload is
    /// variable-length (up to kMaxLevels per side).
    void publish(const md::MdOrderBook& book);

    /// \brief Monotonic sequence number stamped on the most recent message.
    ///        Useful for downstream gap detection and metrics.
    [[nodiscard]] uint64_t current_seq() const { return seq_.load(std::memory_order_relaxed); }

    /// \brief Count of messages dropped due to back-pressure or oversize payload.
    ///        Sampled by MdGatewayService's idle-loop reporter every ~10µs.
    [[nodiscard]] uint64_t drop_count() const { return drops_.load(std::memory_order_relaxed); }

private:
    void record_drop(uint64_t instrument_id, const char* label);

    bpt::common::aeron::Publisher publisher_;

    /// Cache-line isolated. The publisher thread does a `lock add` on
    /// seq_ every publish; MdGatewayService's reporter reads drop_count()
    /// every ~10µs idle iteration. Separating them keeps the reporter's
    /// read out of the publisher's hot cache line.
    alignas(64) std::atomic<uint64_t> seq_{0};
    alignas(64) std::atomic<uint64_t> drops_{0};
};

}  // namespace bpt::md_gateway::messaging

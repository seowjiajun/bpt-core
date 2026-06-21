#pragma once

/// \file
/// \brief Concrete MD publisher — validates, encodes, and offers BBO /
///        Trade / OrderBook on stream 2002 (MdGateway → Strategy).
///
/// Validation and the drop-rate circuit breaker live inside MdPublisher
/// itself (post-fold — they used to live in a separate ValidatingPublisher
/// decorator). The wrapper was N=1 with the wrapee, shared all state +
/// lifetime + thread affinity, and the abstraction was already leaky
/// (drop_count summed across layers). Folding deletes one class without
/// changing the runtime behaviour or the tick path's inlinability.
///
/// One MdPublisher per adapter (per publisher thread). Validator's
/// per-instrument state is thread-confined to that adapter; the
/// underlying aeron::Publisher's mutex becomes uncontended.
///
/// No virtual interface: venue decoders are templated on the inner
/// publisher type, so the chain
///     decoder → md_pub_->publish() → Publisher::publish<T> (zero-copy SBE)
/// inlines all the way down.

#include "md_gateway/md/md_encoder.h"
#include "md_gateway/md/md_publisher_concept.h"
#include "md_gateway/md/md_types.h"
#include "md_gateway/md/md_validator.h"
#include "md_gateway/md/validation_drop_breaker.h"

#include <Aeron.h>

#include <atomic>
#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/stream_config.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::md_gateway::messaging {

/// \brief Publishes normalised MD on the MdGateway→Strategy stream.
///
/// BBO and Trade use the zero-copy publish<T> path — SBE encodes
/// directly into the Aeron log buffer via tryClaim. OrderBook stays on
/// offer() because its payload is variable-length.
///
/// Not thread-safe — one instance per adapter (publisher) thread.
/// Aeron permits multiple publications on the same (channel, stream_id);
/// the subscriber sees N session-ids interleaved and assembles them
/// per-image.
class MdPublisher {
public:
    /// \brief Construct.
    /// \param aeron                     shared Aeron client (typically the bus-built singleton)
    /// \param stream                    Aeron channel + stream id (md.feed, 2002 in prod)
    /// \param max_price_deviation_pct   validator mid-deviation guard threshold (10.0 = 10%)
    /// \param breaker_cfg               rolling-window drop-rate breaker config (default-disabled)
    /// \param adapter_name              shown in breaker trip logs (e.g. "BINANCE")
    MdPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                const bpt::common::config::StreamConfig& stream,
                double max_price_deviation_pct,
                md::ValidationDropBreaker::Config breaker_cfg,
                std::string adapter_name);

    /// \brief Publish a top-of-book quote. Zero-copy via tryClaim.
    void publish(const md::MdBbo& bbo);
    /// \brief Publish a trade print. Zero-copy via tryClaim.
    void publish(const md::MdTrade& trade);
    /// \brief Publish an order-book snapshot. Variable-size payload via offer().
    void publish(const md::MdOrderBook& book);

    /// \brief Forget per-instrument validator state — call on adapter reconnect.
    void reset_validator() { validator_.reset(); }

    /// \brief Monotonic per-adapter sequence number stamped on the most recent message.
    [[nodiscard]] uint64_t current_seq() const { return seq_.load(std::memory_order_relaxed); }

    /// \brief Back-pressure / oversize drops at the Aeron-offer layer.
    [[nodiscard]] uint64_t drop_count() const { return backpressure_drops_.load(std::memory_order_relaxed); }

    /// \brief Successful publishes (validation passed and offer succeeded).
    [[nodiscard]] uint64_t published() const noexcept { return published_.load(std::memory_order_relaxed); }

    /// \brief Drops at the validation/breaker layer (never reached Aeron).
    [[nodiscard]] uint64_t validation_drops() const noexcept {
        return validation_drops_.load(std::memory_order_relaxed);
    }

    /// \brief True if the drop-rate breaker has latched.
    [[nodiscard]] bool breaker_tripped() const noexcept { return breaker_.tripped(); }

private:
    /// \brief Fast pre-check: returns false (and counts a validation drop)
    ///        if the breaker is already tripped, so validate() is skipped.
    bool check_breaker_or_drop();

    /// \brief Feed the validation outcome into the breaker, log on latch,
    ///        and bump validation_drops_ if the message must be dropped.
    void record_validation_and_breaker(bool is_drop);

    void record_backpressure_drop(uint64_t instrument_id, const char* label);

    bpt::common::aeron::Publisher publisher_;
    md::MdValidator validator_;
    md::ValidationDropBreaker breaker_;
    std::string adapter_name_;

    /// Cache-line isolated. Producer thread increments these per publish;
    /// reporter polls every ~5 s. Separating each from neighbouring state
    /// keeps reporter reads off the producer's hot line.
    alignas(64) std::atomic<uint64_t> seq_{0};
    alignas(64) std::atomic<uint64_t> published_{0};
    alignas(64) std::atomic<uint64_t> backpressure_drops_{0};
    alignas(64) std::atomic<uint64_t> validation_drops_{0};
};

// Self-verification: this concrete satisfies the contract its consumers
// (venue decoders) constrain on.
static_assert(md::MdPublisher<MdPublisher>);

}  // namespace bpt::md_gateway::messaging

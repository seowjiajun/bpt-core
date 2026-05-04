#pragma once

/// @file
/// Shared Aeron subscription wrapper.
///
/// Owns the Subscription + FragmentAssembler pair that every service
/// wires up by hand today, so callers write:
///
/// @code
///   Subscriber sub(aeron, channel, stream_id,
///                  [this](auto& buf, auto offset, auto len, auto& hdr) {
///                      dispatch(buf, offset, len, hdr);
///                  });
///   ...
///   while (running) {
///       if (sub.poll() == 0) std::this_thread::yield();
///   }
/// @endcode
///
/// instead of the ~15-line manual dance:
/// @code
///   long id = aeron->addSubscription(...);
///   sub = ...;
///   assembler_ = std::make_unique<FragmentAssembler>(handler_lambda);
///   sub_->poll(assembler_->handler(), 16);
/// @endcode
///
/// Minimal intentionally. Does NOT do templateId-based dispatch — that
/// would introduce a new programming model (variant dispatch, type
/// erasure, etc.) that's worth designing in its own pass. Current
/// callers have a handful of templateId branches in their handler
/// lambda and that's fine.
///
/// Thread-safety: a single Subscriber is meant to be polled from one
/// thread (Aeron's subscription poll is not designed for concurrent
/// consumers anyway). No internal mutex.

#include "bpt_common/aeron/aeron_utils.h"
#include "bpt_common/aeron/chaos_filter.h"
#include "bpt_common/aeron/fragment_handler.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::common::aeron {

class Subscriber {
public:
    Subscriber(std::shared_ptr<::aeron::Aeron> aeron,
               std::string channel,
               std::int32_t stream_id,
               FragmentHandler handler)
        : channel_(std::move(channel)),
          stream_id_(stream_id),
          subscription_(wait_for_subscription(std::move(aeron), channel_, stream_id)),
          assembler_(std::make_unique<::aeron::FragmentAssembler>(
              ChaosRegistry::wrap(stream_id, std::move(handler)))) {}

    /// Non-copyable, non-movable.
    ///
    /// The stored handler almost always captures `this` of the owning
    /// class; moving a Subscriber while the handler still carries the
    /// old pointer is a dangling-capture time-bomb (fires under
    /// vector-realloc, return-by-value, etc.). Classes that hold a
    /// Subscriber must also be non-movable; wrap in std::unique_ptr if
    /// you need movability at the owner level — the unique_ptr moves,
    /// the Subscriber object stays put.
    Subscriber(const Subscriber&)            = delete;
    Subscriber& operator=(const Subscriber&) = delete;
    Subscriber(Subscriber&&)                 = delete;
    Subscriber& operator=(Subscriber&&)      = delete;

    /// Poll up to `limit` fragments.
    ///
    /// Returns the number of fragments actually consumed — a poll
    /// returning 0 is the caller's cue to yield or sleep.
    int poll(int limit = 16) {
        return subscription_->poll(assembler_->handler(), limit);
    }

    [[nodiscard]] bool is_connected() const noexcept {
        return subscription_ && subscription_->isConnected();
    }
    [[nodiscard]] const std::string& channel() const noexcept { return channel_; }
    [[nodiscard]] std::int32_t stream_id() const noexcept { return stream_id_; }

    /// Escape hatch for callers that need the raw Subscription.
    ///
    /// Not encouraged, but sometimes the Aeron API surface area is
    /// wider than what poll() exposes (e.g. image count queries).
    [[nodiscard]] ::aeron::Subscription& raw() noexcept { return *subscription_; }

private:
    std::string channel_;
    std::int32_t stream_id_;
    std::shared_ptr<::aeron::Subscription> subscription_;
    std::unique_ptr<::aeron::FragmentAssembler> assembler_;
};

}  // namespace bpt::common::aeron

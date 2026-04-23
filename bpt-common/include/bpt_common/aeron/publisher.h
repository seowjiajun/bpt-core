#pragma once

// Shared Aeron publication wrapper. Replaces the per-publisher
// hand-rolled offer/retry loops scattered across bpt-order-gateway,
// bpt-md-gateway, bpt-strategy, bpt-analytics, bpt-refdata, bpt-book,
// etc. — each of which was writing subtly different variants of
// "spin on offer() < 0 until it succeeds or forever."
//
// The old `spin forever on negative return` pattern deadlocks if no
// subscriber ever connects (Aeron returns NOT_CONNECTED indefinitely).
// Several of our publishers had this footgun baked in; the only reason
// it wasn't surfacing was that every stream happened to have a
// subscriber in practice. bpt-book tripped on this first: no downstream
// had subscribed to stream 6001 yet, and the whole poll loop hung.
//
// This helper centralises the decision:
//   - BACK_PRESSURED / ADMIN_ACTION → yield + retry (always)
//   - NOT_CONNECTED / PUBLICATION_CLOSED → policy-driven drop
//   - Successful offer → return true
//
// Thread-safe (per-publisher mutex around offer). Callers that do
// their own synchronisation pay a small extra lock cost — not
// measurable at the cadences we run at.

#include "bpt_common/aeron/aeron_utils.h"

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bpt::common::aeron {

class Publisher {
public:
    // Policy controls what happens when offer() returns a negative
    // value. All policies retry on BACK_PRESSURED / ADMIN_ACTION
    // (yield + loop); the difference is how NOT_CONNECTED / CLOSED
    // are treated.
    enum class Policy {
        // Retry indefinitely on back-pressure; drop immediately on
        // NOT_CONNECTED or PUBLICATION_CLOSED. Safe default for most
        // publishers — waiting for a subscriber that may never come
        // blocks the caller's thread forever.
        kRetryOnBackpressure,

        // Single attempt. Any negative return → drop. For
        // latency-critical fast-path publishers (e.g. MdOrderBook)
        // where waiting means stale data is better avoided.
        kDropAlways,

        // Retry up to max_retries on back-pressure, then drop with a
        // warning log. Also drops immediately on NOT_CONNECTED /
        // CLOSED. For snapshot / heartbeat publishers where
        // stale-is-fine and the next tick republishes.
        kBoundedRetry,
    };

    Publisher(std::shared_ptr<::aeron::Aeron> aeron,
              std::string channel,
              std::int32_t stream_id,
              Policy default_policy = Policy::kRetryOnBackpressure,
              int max_retries = 1000)
        : channel_(std::move(channel)),
          stream_id_(stream_id),
          default_policy_(default_policy),
          max_retries_(max_retries),
          publication_(wait_for_publication(std::move(aeron), channel_, stream_id)) {}

    // Offer a pre-encoded buffer slice. Returns true if successfully
    // offered, false if dropped (NOT_CONNECTED / CLOSED / retry
    // budget exhausted). Never throws.
    bool offer(const ::aeron::AtomicBuffer& buf,
               std::int32_t offset,
               std::int32_t length) {
        return offer(buf, offset, length, default_policy_);
    }

    bool offer(const ::aeron::AtomicBuffer& buf,
               std::int32_t offset,
               std::int32_t length,
               Policy policy) {
        std::lock_guard<std::mutex> lock(mutex_);
        const int max = (policy == Policy::kDropAlways)  ? 1
                      : (policy == Policy::kBoundedRetry) ? max_retries_
                                                           : -1;  // unbounded
        for (int attempt = 0; max < 0 || attempt < max; ++attempt) {
            const std::int64_t rc = publication_->offer(buf, offset, length);
            if (rc >= 0)
                return true;
            // Terminal negatives — further retries can never succeed.
            if (rc == ::aeron::NOT_CONNECTED || rc == ::aeron::PUBLICATION_CLOSED)
                return false;
            // MAX_POSITION_EXCEEDED means the log term filled; retry
            // is pointless until operators rotate the log — same
            // terminal treatment.
            if (rc == ::aeron::MAX_POSITION_EXCEEDED)
                return false;
            // BACK_PRESSURED / ADMIN_ACTION — transient, yield and retry.
            std::this_thread::yield();
        }
        return false;  // kBoundedRetry + kDropAlways fall through here
    }

    // Exposed so callers can special-case their own telemetry or
    // short-circuit encoding when no subscriber is connected.
    [[nodiscard]] bool is_connected() const noexcept {
        return publication_ && publication_->isConnected();
    }

    [[nodiscard]] const std::string& channel() const noexcept { return channel_; }
    [[nodiscard]] std::int32_t stream_id() const noexcept { return stream_id_; }

private:
    std::string channel_;
    std::int32_t stream_id_;
    Policy default_policy_;
    int max_retries_;
    std::shared_ptr<::aeron::Publication> publication_;
    std::mutex mutex_;
};

}  // namespace bpt::common::aeron

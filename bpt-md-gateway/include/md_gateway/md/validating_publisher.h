#pragma once

/// \file
/// \brief Validation + circuit-breaker decorator wrapping the concrete MdPublisher.
///
/// CRTP-templated on `Inner` so the chain
///     adapter decoder → ValidatingPublisher<Inner> → Inner
/// is fully vtable-free — every `publish()` is a direct, inlinable call.
/// Maintains atomic counters (relaxed) the app polls every ~5 s to push
/// deltas to Prometheus without adding per-message overhead beyond a
/// single fetch_add.
///
/// Hosts the per-adapter ValidationDropBreaker: once the drop ratio
/// over the rolling window crosses the configured threshold, all
/// further publishes short-circuit (counted as drops; `Inner::publish`
/// never called) until the process restarts. A feed that has gone bad
/// (schema drift, crossed ladders) is suppressed instead of being
/// forwarded as corrupt data.

#include "md_gateway/md/md_validator.h"
#include "md_gateway/md/validation_drop_breaker.h"

#include <atomic>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <cstdint>

namespace bpt::md_gateway::md {

/// \brief CRTP decorator: validate-then-forward + drop-rate breaker.
///
/// Not thread-safe for the validator state — one instance per adapter
/// (publisher) thread, matching the single-writer guarantee of MdValidator.
///
/// `Inner` must expose:
///     - `void publish(const MdBbo&)`
///     - `void publish(const MdTrade&)`
///     - `void publish(const MdOrderBook&)`
///     - `uint64_t drop_count() const`
template <class Inner>
class ValidatingPublisher {
public:
    ValidatingPublisher(Inner& inner, MdValidator& validator, const char* adapter_name = "unknown")
        : inner_(inner),
          validator_(validator),
          adapter_name_(adapter_name),
          breaker_({}) {}

    /// \brief Configure the drop-rate breaker.
    ///
    /// Safe to call before start. Re-entry after start is not supported
    /// — breaker state is not atomic-reset.
    void set_drop_breaker_config(ValidationDropBreaker::Config cfg) { breaker_ = ValidationDropBreaker(cfg); }

    void publish(const MdBbo& bbo) { forward(bbo); }
    void publish(const MdTrade& trade) { forward(trade); }
    void publish(const MdOrderBook& book) { forward(book); }

    /// \brief Drops at this layer (validation rejections + breaker latch) plus drops downstream.
    ///
    /// Layered semantic: a subscriber reading this port sees every
    /// message that failed to reach the wire, regardless of which
    /// layer rejected it.
    [[nodiscard]] uint64_t drop_count() const { return inner_.drop_count() + drops_.load(std::memory_order_relaxed); }

    [[nodiscard]] uint64_t published() const noexcept { return published_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool breaker_tripped() const noexcept { return breaker_.tripped(); }

private:
    template <typename T>
    void forward(const T& msg) {
        if (breaker_.tripped()) {
            drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const bool is_drop = (validator_.validate(msg) != ValidationResult::OK);
        const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
        const bool was_tripped = breaker_.tripped();
        breaker_.record(is_drop, now_ns);
        if (!was_tripped && breaker_.tripped()) {
            bpt::common::log::error(
                "{} VALIDATION-DROP BREAKER TRIPPED — {}/{} publishes "
                "dropped in last {}s (threshold {:.1f}%). Publishing suppressed. "
                "Restart service after human review to resume.",
                adapter_name_,
                breaker_.drops_in_window(),
                breaker_.total_in_window(),
                breaker_.config().window_ns / 1'000'000'000ULL,
                breaker_.config().threshold_pct);
        }

        if (!is_drop) {
            inner_.publish(msg);
            published_.fetch_add(1, std::memory_order_relaxed);
        } else {
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    Inner& inner_;
    MdValidator& validator_;
    const char* adapter_name_;
    ValidationDropBreaker breaker_;
    /// Cache-line isolated. `published_` is incremented every successful
    /// forward (the 99 %+ path). MdGatewayService's reporter reads both
    /// counters every ~5 s; separating them from each other and from
    /// the upstream class state keeps the producer's hot line untouched
    /// by reporter polling.
    alignas(64) std::atomic<uint64_t> published_{0};
    alignas(64) std::atomic<uint64_t> drops_{0};
};

}  // namespace bpt::md_gateway::md

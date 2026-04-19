#pragma once

#include "md_gateway/md/md_validator.h"
#include "md_gateway/md/validation_drop_breaker.h"
#include "md_gateway/messaging/i_md_publisher.h"

#include <atomic>
#include <cstdint>
#include <yggdrasil/logging.h>
#include <yggdrasil/util/tsc_clock.h>

namespace bpt::md_gateway::md {

// Decorator that validates normalised market-data structs before forwarding
// them to an inner IMdPublisher.  Invalid structs are silently dropped (the
// validator logs a warning).
//
// Maintains monotonic published_ and drops_ counters (relaxed atomics) so the
// app can poll them and push deltas to Prometheus without adding per-message
// overhead beyond a single fetch_add.
//
// Also hosts the per-adapter ValidationDropBreaker: once the drop rate
// over the rolling window crosses the configured threshold, all further
// publishes short-circuit (counted as drops; inner_ never called) until
// the process restarts. Intended signal is a feed that has gone bad
// (schema drift, crossed ladders) where suppressing output is safer
// than forwarding corrupt data.
//
// Not thread-safe for the validator state — one instance per adapter
// (publisher) thread, matching the single-writer guarantee of MdValidator.
class ValidatingPublisher final : public messaging::IMdPublisher {
public:
    ValidatingPublisher(messaging::IMdPublisher& inner, MdValidator& validator,
                        const char* adapter_name = "unknown")
        : inner_(inner),
          validator_(validator),
          adapter_name_(adapter_name),
          breaker_({}) {}

    // Configure the drop-rate breaker. Safe to call before start; re-entry
    // after start is not supported (breaker state is not atomic-reset).
    void set_drop_breaker_config(ValidationDropBreaker::Config cfg) {
        breaker_ = ValidationDropBreaker(cfg);
    }

    void publish(const MdBbo& bbo) override { forward(bbo); }
    void publish(const MdTrade& trade) override { forward(trade); }
    void publish(const MdOrderBook& book) override { forward(book); }

    [[nodiscard]] uint64_t published() const noexcept { return published_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }
    [[nodiscard]] bool breaker_tripped() const noexcept { return breaker_.tripped(); }

private:
    // Shared forwarding path — templated on the md struct type so we
    // don't triplicate the breaker-gate / logging logic.
    template <typename T>
    void forward(const T& msg) {
        if (breaker_.tripped()) {
            // Already latched — skip both validation and forward. Still
            // counts toward drops_ so the Prometheus counter reflects
            // everything the adapter tried to publish post-trip.
            drops_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        const bool is_drop = (validator_.validate(msg) != ValidationResult::OK);
        const uint64_t now_ns = ygg::util::TscClock::now_epoch_ns();
        const bool was_tripped = breaker_.tripped();
        breaker_.record(is_drop, now_ns);
        if (!was_tripped && breaker_.tripped()) {
            ygg::log::error(
                "[MdGateway] {} VALIDATION-DROP BREAKER TRIPPED — {}/{} publishes "
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

    messaging::IMdPublisher& inner_;
    MdValidator& validator_;
    const char* adapter_name_;
    ValidationDropBreaker breaker_;
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> drops_{0};
};

}  // namespace bpt::md_gateway::md

#pragma once

#include "md_gateway/md/md_validator.h"
#include "md_gateway/messaging/i_md_publisher.h"

#include <atomic>
#include <cstdint>

namespace bpt::md_gateway::md {

// Decorator that validates normalised market-data structs before forwarding
// them to an inner IMdPublisher.  Invalid structs are silently dropped (the
// validator logs a warning).
//
// Maintains monotonic published_ and drops_ counters (relaxed atomics) so the
// app can poll them and push deltas to Prometheus without adding per-message
// overhead beyond a single fetch_add.
//
// Not thread-safe for the validator state — one instance per adapter
// (publisher) thread, matching the single-writer guarantee of MdValidator.
class ValidatingPublisher final : public messaging::IMdPublisher {
public:
    ValidatingPublisher(messaging::IMdPublisher& inner, MdValidator& validator)
        : inner_(inner),
          validator_(validator) {}

    void publish(const MdBbo& bbo) override {
        if (validator_.validate(bbo) == ValidationResult::OK) {
            inner_.publish(bbo);
            published_.fetch_add(1, std::memory_order_relaxed);
        } else {
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void publish(const MdTrade& trade) override {
        if (validator_.validate(trade) == ValidationResult::OK) {
            inner_.publish(trade);
            published_.fetch_add(1, std::memory_order_relaxed);
        } else {
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void publish(const MdOrderBook& book) override {
        if (validator_.validate(book) == ValidationResult::OK) {
            inner_.publish(book);
            published_.fetch_add(1, std::memory_order_relaxed);
        } else {
            drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] uint64_t published() const noexcept { return published_.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }

private:
    messaging::IMdPublisher& inner_;
    MdValidator& validator_;
    std::atomic<uint64_t> published_{0};
    std::atomic<uint64_t> drops_{0};
};

}  // namespace bpt::md_gateway::md

#pragma once

#include <messages/ExchangeId.h>
#include <messages/RejectReason.h>

#include <atomic>
#include <cstdint>
#include <expected>

namespace bpt::order_gateway::risk {

// All limits stored as atomics so they can be updated at runtime without
// restart. All checks execute synchronously on the hot path — zero allocation,
// lock-free.
class RiskChecker {
public:
    explicit RiskChecker(double max_order_size_usd,
                         double max_notional_per_order_usd,
                         uint32_t max_open_orders_per_venue,
                         uint32_t max_orders_per_second);

    // Returns OK or the rejection reason. Called before sending any order.
    // price and quantity are scaled integers (divide by 1e8 for actual value).
    [[nodiscard]] std::expected<void, bpt::messages::RejectReason::Value> check(
        bpt::messages::ExchangeId::Value exchange,
        uint64_t instrument_id,
        int64_t price,
        uint64_t quantity,
        uint64_t order_id);

    // Called when an order is acknowledged/filled/cancelled to decrement
    // counters.
    void on_order_closed(bpt::messages::ExchangeId::Value exchange);

    // Kill switch — atomically disables all trading.
    void set_trading_enabled(bool enabled) noexcept;
    [[nodiscard]] bool trading_enabled() const noexcept;

    // Total open orders across all venues — O(1), atomic load per venue.
    [[nodiscard]] uint32_t total_open_orders() const noexcept;

    // Runtime limit updates (no restart required).
    void set_max_order_size_usd(double v) noexcept;
    void set_max_notional_per_order_usd(double v) noexcept;
    void set_max_open_orders_per_venue(uint32_t v) noexcept;
    void set_max_orders_per_second(uint32_t v) noexcept;

private:
    static constexpr int kMaxVenues = 4;
    static constexpr double kScale = 1e8;

    std::atomic<bool> trading_enabled_{true};
    // Use bit-cast trick to store doubles atomically (IEEE 754 double -> uint64
    // -> atomic)
    std::atomic<uint64_t> max_order_size_usd_bits_;
    std::atomic<uint64_t> max_notional_bits_;
    std::atomic<uint32_t> max_open_orders_per_venue_;
    std::atomic<uint32_t> max_orders_per_second_;

    // Per-venue open order counters.
    std::atomic<uint32_t> open_orders_[kMaxVenues]{};

    // Rate limiter: token bucket approximation using atomics.
    // orders_this_second_ resets when timestamp_s_ changes.
    std::atomic<uint64_t> rate_window_s_{0};
    std::atomic<uint32_t> orders_this_window_{0};

    // Seen order IDs for duplicate detection (bounded ring — last 1024 IDs).
    static constexpr std::size_t kDupRingSize = 1024;
    std::atomic<uint64_t> dup_ring_[kDupRingSize]{};
    std::atomic<std::size_t> dup_head_{0};
};

}  // namespace bpt::order_gateway::risk

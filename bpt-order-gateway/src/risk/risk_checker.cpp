#include "order_gateway/risk/risk_checker.h"

#include <chrono>
#include <cstring>
#include <expected>

namespace bpt::order_gateway::risk {

namespace {

// Bit-cast helpers: store/load doubles via uint64 atomics.
uint64_t double_to_bits(double v) noexcept {
    uint64_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

double bits_to_double(uint64_t bits) noexcept {
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

uint64_t current_second() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

}  // namespace

RiskChecker::RiskChecker(double max_order_size_usd,
                         double max_notional_per_order_usd,
                         uint32_t max_open_orders_per_venue,
                         uint32_t max_orders_per_second)
    : max_order_size_usd_bits_(double_to_bits(max_order_size_usd)),
      max_notional_bits_(double_to_bits(max_notional_per_order_usd)),
      max_open_orders_per_venue_(max_open_orders_per_venue),
      max_orders_per_second_(max_orders_per_second) {}

std::expected<void, bpt::messages::RejectReason::Value> RiskChecker::check(
    bpt::messages::ExchangeId::Value exchange,
    uint64_t /*instrument_id*/,
    int64_t price,
    uint64_t quantity,
    uint64_t order_id) {
    using RR = bpt::messages::RejectReason;

    // 1. Kill switch check
    if (!trading_enabled_.load(std::memory_order_acquire))
        return std::unexpected(RR::RISK_REJECTED);

    // 2. Duplicate order ID check (ring buffer scan — last kDupRingSize IDs)
    for (std::size_t i = 0; i < kDupRingSize; ++i) {
        if (dup_ring_[i].load(std::memory_order_relaxed) == order_id)
            return std::unexpected(RR::DUPLICATE_ORDER_ID);
    }

    // 3. Open orders per venue check
    const int venue_idx = static_cast<int>(exchange) < kMaxVenues ? static_cast<int>(exchange) : 0;
    const uint32_t max_open = max_open_orders_per_venue_.load(std::memory_order_relaxed);
    if (open_orders_[venue_idx].load(std::memory_order_relaxed) >= max_open)
        return std::unexpected(RR::RISK_REJECTED);

    // 4. Rate limit check (per-second token bucket approximation)
    uint64_t now_s = current_second();
    uint64_t window = rate_window_s_.load(std::memory_order_relaxed);
    if (window != now_s) {
        // New second — try to reset the window.
        if (rate_window_s_.compare_exchange_strong(window,
                                                   now_s,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
            orders_this_window_.store(0, std::memory_order_relaxed);
        }
    }
    const uint32_t max_rate = max_orders_per_second_.load(std::memory_order_relaxed);
    const uint32_t count = orders_this_window_.fetch_add(1, std::memory_order_relaxed);
    if (count >= max_rate) {
        orders_this_window_.fetch_sub(1, std::memory_order_relaxed);
        return std::unexpected(RR::RATE_LIMITED);
    }

    // 5. Order size and notional checks
    if (price <= 0 && quantity > 0) {
        // MARKET order — skip price check
    } else {
        double price_usd = static_cast<double>(price) / kScale;
        double qty_actual = static_cast<double>(quantity) / kScale;

        double order_size = price_usd * qty_actual;
        double max_size = bits_to_double(max_order_size_usd_bits_.load(std::memory_order_relaxed));
        if (order_size > max_size) {
            orders_this_window_.fetch_sub(1, std::memory_order_relaxed);
            return std::unexpected(RR::RISK_REJECTED);
        }

        double max_notional = bits_to_double(max_notional_bits_.load(std::memory_order_relaxed));
        if (order_size > max_notional) {
            orders_this_window_.fetch_sub(1, std::memory_order_relaxed);
            return std::unexpected(RR::RISK_REJECTED);
        }
    }

    // All checks passed — record the order ID and increment open orders counter
    std::size_t head = dup_head_.fetch_add(1, std::memory_order_relaxed) % kDupRingSize;
    dup_ring_[head].store(order_id, std::memory_order_relaxed);
    open_orders_[venue_idx].fetch_add(1, std::memory_order_relaxed);

    return {};
}

void RiskChecker::on_order_closed(bpt::messages::ExchangeId::Value exchange) {
    const int venue_idx = static_cast<int>(exchange) < kMaxVenues ? static_cast<int>(exchange) : 0;
    uint32_t prev = open_orders_[venue_idx].load(std::memory_order_relaxed);
    // Avoid underflow
    while (prev > 0) {
        if (open_orders_[venue_idx].compare_exchange_weak(prev,
                                                          prev - 1,
                                                          std::memory_order_relaxed,
                                                          std::memory_order_relaxed))
            break;
    }
}

void RiskChecker::set_trading_enabled(bool enabled) noexcept {
    trading_enabled_.store(enabled, std::memory_order_release);
}

bool RiskChecker::trading_enabled() const noexcept {
    return trading_enabled_.load(std::memory_order_acquire);
}

void RiskChecker::set_max_order_size_usd(double v) noexcept {
    max_order_size_usd_bits_.store(double_to_bits(v), std::memory_order_relaxed);
}

void RiskChecker::set_max_notional_per_order_usd(double v) noexcept {
    max_notional_bits_.store(double_to_bits(v), std::memory_order_relaxed);
}

void RiskChecker::set_max_open_orders_per_venue(uint32_t v) noexcept {
    max_open_orders_per_venue_.store(v, std::memory_order_relaxed);
}

void RiskChecker::set_max_orders_per_second(uint32_t v) noexcept {
    max_orders_per_second_.store(v, std::memory_order_relaxed);
}

uint32_t RiskChecker::total_open_orders() const noexcept {
    uint32_t total = 0;
    for (int i = 0; i < kMaxVenues; ++i)
        total += open_orders_[i].load(std::memory_order_relaxed);
    return total;
}

}  // namespace bpt::order_gateway::risk

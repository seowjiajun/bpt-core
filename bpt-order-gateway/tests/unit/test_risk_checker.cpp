// Unit tests for RiskChecker — all pure logic, no network, no Aeron.
#include "order_gateway/risk/risk_checker.h"

#include <messages/ExchangeId.h>
#include <messages/RejectReason.h>

#include <gtest/gtest.h>

using namespace bpt::order_gateway::risk;
using EX = bpt::messages::ExchangeId;
using RR = bpt::messages::RejectReason;

// ── Helpers ───────────────────────────────────────────────────────────────────

static constexpr int64_t kScaledPrice = static_cast<int64_t>(100.0 * 1e8);  // $100
static constexpr uint64_t kScaledQty = static_cast<uint64_t>(1.0 * 1e8);    // 1 unit
// Notional = $100 * 1 = $100 — within a $1000 limit

static RiskChecker make_checker() {
    return RiskChecker(
        /*max_order_size_usd=*/1000.0,
        /*max_notional_per_order=*/5000.0,
        /*max_open_orders_per_venue=*/5,
        /*max_orders_per_second=*/100);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(RiskCheckerTest, ValidOrderPasses) {
    auto rc = make_checker();
    auto result = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 42ULL);
    EXPECT_TRUE(result.has_value());
}

TEST(RiskCheckerTest, KillSwitchRejectsOrder) {
    auto rc = make_checker();
    rc.set_trading_enabled(false);
    auto result = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 43ULL);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RR::RISK_REJECTED);
}

TEST(RiskCheckerTest, TradingEnabledDefaultIsTrue) {
    auto rc = make_checker();
    EXPECT_TRUE(rc.trading_enabled());
}

TEST(RiskCheckerTest, SetTradingEnabledRoundTrip) {
    auto rc = make_checker();
    rc.set_trading_enabled(false);
    EXPECT_FALSE(rc.trading_enabled());
    rc.set_trading_enabled(true);
    EXPECT_TRUE(rc.trading_enabled());
}

TEST(RiskCheckerTest, DuplicateOrderIdRejected) {
    auto rc = make_checker();
    // First use of order_id=100 passes
    auto r1 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 100ULL);
    EXPECT_TRUE(r1.has_value());

    // Second use of same order_id=100 is rejected
    auto r2 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 100ULL);
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error(), RR::DUPLICATE_ORDER_ID);
}

TEST(RiskCheckerTest, OpenOrdersPerVenueAtMaxRejectsNext) {
    // max=2 to make the test fast
    RiskChecker rc(1000.0, 5000.0, 2, 100);

    auto r1 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 1ULL);
    EXPECT_TRUE(r1.has_value());
    auto r2 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 2ULL);
    EXPECT_TRUE(r2.has_value());

    // Third order should be rejected
    auto r3 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 3ULL);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error(), RR::RISK_REJECTED);
}

TEST(RiskCheckerTest, OnOrderClosedDecrementsCounter) {
    RiskChecker rc(1000.0, 5000.0, 2, 100);

    rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 10ULL);
    rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 11ULL);

    // At limit — 3rd should fail
    auto r3 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 12ULL);
    EXPECT_FALSE(r3.has_value());

    // Close one order
    rc.on_order_closed(EX::BINANCE);

    // Now 3rd should pass (using new ID to avoid dup rejection)
    auto r4 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 13ULL);
    EXPECT_TRUE(r4.has_value());
}

TEST(RiskCheckerTest, RateLimitExceeded) {
    // max 2 orders per second
    RiskChecker rc(1000.0, 5000.0, 50, 2);

    auto r1 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 20ULL);
    EXPECT_TRUE(r1.has_value());
    auto r2 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 21ULL);
    EXPECT_TRUE(r2.has_value());
    auto r3 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 22ULL);
    ASSERT_FALSE(r3.has_value());
    EXPECT_EQ(r3.error(), RR::RATE_LIMITED);
}

TEST(RiskCheckerTest, OversizedOrderRejected) {
    // max order size = $50
    RiskChecker rc(50.0, 5000.0, 50, 100);

    // Order: price=$100, qty=1 → notional=$100 > $50 limit
    auto result = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 30ULL);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error(), RR::RISK_REJECTED);
}

TEST(RiskCheckerTest, OrderWithinSizePassess) {
    // max order size = $200
    RiskChecker rc(200.0, 5000.0, 50, 100);

    // Order: price=$100, qty=1 → notional=$100 < $200 limit
    auto result = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 31ULL);
    EXPECT_TRUE(result.has_value());
}

TEST(RiskCheckerTest, DifferentVenuesHaveIndependentCounters) {
    RiskChecker rc(1000.0, 5000.0, 1, 100);

    // Fill BINANCE limit
    auto r1 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 40ULL);
    EXPECT_TRUE(r1.has_value());

    // BINANCE should now be rejected
    auto r2 = rc.check(EX::BINANCE, 1ULL, kScaledPrice, kScaledQty, 41ULL);
    EXPECT_FALSE(r2.has_value());

    // OKX should still pass (different venue counter)
    auto r3 = rc.check(EX::OKX, 1ULL, kScaledPrice, kScaledQty, 42ULL);
    EXPECT_TRUE(r3.has_value());
}

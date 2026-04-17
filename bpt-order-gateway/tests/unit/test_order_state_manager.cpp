// Unit tests for OrderStateManager — pure logic, no network, no Aeron.
#include "order_gateway/order/order_state_manager.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>

#include <gtest/gtest.h>
#include <vector>

using namespace bpt::order_gateway::order;
using EX = bpt::messages::ExchangeId;
using OS = bpt::messages::OrderSide;
using OT = bpt::messages::OrderType;

static OrderState make_order(uint64_t id, EX::Value exchange = EX::BINANCE) {
    OrderState st;
    st.order_id = id;
    st.exchange_id = exchange;
    st.instrument_id = 100ULL;
    st.side = OS::BUY;
    st.order_type = OT::LIMIT;
    st.price = 300000'00000000LL;  // $30000 scaled
    st.quantity = 1'00000000ULL;   // 1 unit scaled
    st.remaining_qty = st.quantity;
    st.lifecycle = OrderLifecycle::PENDING;
    st.created_ns = 1000ULL;
    st.last_update_ns = 1000ULL;
    return st;
}

// ── Insert ────────────────────────────────────────────────────────────────────

TEST(OrderStateManagerTest, InsertAndGet) {
    OrderStateManager mgr;
    auto st = make_order(1ULL);
    EXPECT_TRUE(mgr.insert(st));

    const auto* got = mgr.get(1ULL);
    ASSERT_NE(got, nullptr);
    EXPECT_EQ(got->order_id, 1ULL);
    EXPECT_EQ(got->lifecycle, OrderLifecycle::PENDING);
}

TEST(OrderStateManagerTest, InsertDuplicateFails) {
    OrderStateManager mgr;
    mgr.insert(make_order(1ULL));
    EXPECT_FALSE(mgr.insert(make_order(1ULL)));
}

TEST(OrderStateManagerTest, GetNotFound) {
    OrderStateManager mgr;
    EXPECT_EQ(mgr.get(999ULL), nullptr);
}

// ── Lifecycle transitions ─────────────────────────────────────────────────────

TEST(OrderStateManagerTest, PendingToAcked) {
    OrderStateManager mgr;
    mgr.insert(make_order(1ULL));
    bool ok = mgr.update(1ULL, OrderLifecycle::ACKED, 12345ULL, 0, 100000000ULL, 2000ULL);
    EXPECT_TRUE(ok);

    const auto* st = mgr.get(1ULL);
    EXPECT_EQ(st->lifecycle, OrderLifecycle::ACKED);
    EXPECT_EQ(st->exchange_order_id, 12345ULL);
}

TEST(OrderStateManagerTest, AckedToPartiallyFilled) {
    OrderStateManager mgr;
    mgr.insert(make_order(1ULL));
    mgr.update(1ULL, OrderLifecycle::ACKED, 12345ULL, 0, 100000000ULL, 2000ULL);
    mgr.update(1ULL, OrderLifecycle::PARTIALLY_FILLED, 12345ULL, 50000000ULL, 50000000ULL, 3000ULL);

    const auto* st = mgr.get(1ULL);
    EXPECT_EQ(st->lifecycle, OrderLifecycle::PARTIALLY_FILLED);
    EXPECT_EQ(st->filled_qty, 50000000ULL);
    EXPECT_EQ(st->remaining_qty, 50000000ULL);
}

TEST(OrderStateManagerTest, PartiallyFilledToFilled) {
    OrderStateManager mgr;
    mgr.insert(make_order(1ULL));
    mgr.update(1ULL, OrderLifecycle::ACKED, 12345ULL, 0, 100000000ULL, 2000ULL);
    mgr.update(1ULL, OrderLifecycle::PARTIALLY_FILLED, 12345ULL, 50000000ULL, 50000000ULL, 3000ULL);
    mgr.update(1ULL, OrderLifecycle::FILLED, 12345ULL, 100000000ULL, 0, 4000ULL);

    const auto* st = mgr.get(1ULL);
    EXPECT_EQ(st->lifecycle, OrderLifecycle::FILLED);
    EXPECT_EQ(st->remaining_qty, 0ULL);
}

TEST(OrderStateManagerTest, PendingToCancelled) {
    OrderStateManager mgr;
    mgr.insert(make_order(2ULL));
    mgr.update(2ULL, OrderLifecycle::CANCELLED, 0, 0, 0, 5000ULL);

    const auto* st = mgr.get(2ULL);
    EXPECT_EQ(st->lifecycle, OrderLifecycle::CANCELLED);
}

TEST(OrderStateManagerTest, UpdateNotFound) {
    OrderStateManager mgr;
    bool ok = mgr.update(999ULL, OrderLifecycle::ACKED, 0, 0, 0, 0);
    EXPECT_FALSE(ok);
}

// ── Remove ────────────────────────────────────────────────────────────────────

TEST(OrderStateManagerTest, RemoveTerminalOrder) {
    OrderStateManager mgr;
    mgr.insert(make_order(3ULL));
    mgr.update(3ULL, OrderLifecycle::FILLED, 12345ULL, 100000000ULL, 0, 6000ULL);
    mgr.remove(3ULL);

    EXPECT_EQ(mgr.get(3ULL), nullptr);
}

// ── Open order count ──────────────────────────────────────────────────────────

TEST(OrderStateManagerTest, OpenOrderCountByVenue) {
    OrderStateManager mgr;
    mgr.insert(make_order(10ULL, EX::BINANCE));
    mgr.insert(make_order(11ULL, EX::BINANCE));
    mgr.insert(make_order(12ULL, EX::OKX));

    EXPECT_EQ(mgr.open_order_count(EX::BINANCE), 2u);
    EXPECT_EQ(mgr.open_order_count(EX::OKX), 1u);
    EXPECT_EQ(mgr.total_open_orders(), 3u);
}

TEST(OrderStateManagerTest, FilledOrderNotCountedAsOpen) {
    OrderStateManager mgr;
    mgr.insert(make_order(20ULL, EX::BINANCE));
    mgr.insert(make_order(21ULL, EX::BINANCE));
    mgr.update(20ULL, OrderLifecycle::FILLED, 0, 100000000ULL, 0, 0);
    mgr.remove(20ULL);

    EXPECT_EQ(mgr.open_order_count(EX::BINANCE), 1u);
    EXPECT_EQ(mgr.total_open_orders(), 1u);
}

// ── for_each_open ─────────────────────────────────────────────────────────────

TEST(OrderStateManagerTest, ForEachOpenVisitsOpenOrders) {
    OrderStateManager mgr;
    mgr.insert(make_order(30ULL, EX::BINANCE));
    mgr.insert(make_order(31ULL, EX::BINANCE));
    mgr.insert(make_order(32ULL, EX::OKX));

    // Fill one
    mgr.update(31ULL, OrderLifecycle::FILLED, 0, 100000000ULL, 0, 0);
    mgr.remove(31ULL);

    std::vector<uint64_t> visited;
    mgr.for_each_open([&](OrderState& st) { visited.push_back(st.order_id); });

    EXPECT_EQ(visited.size(), 2u);
    EXPECT_NE(std::find(visited.begin(), visited.end(), 30ULL), visited.end());
    EXPECT_NE(std::find(visited.begin(), visited.end(), 32ULL), visited.end());
    EXPECT_EQ(std::find(visited.begin(), visited.end(), 31ULL), visited.end());
}

// ── check_stale ───────────────────────────────────────────────────────────────

TEST(OrderStateManagerTest, CheckStaleDetectsOldAckedOrder) {
    OrderStateManager mgr;
    mgr.insert(make_order(40ULL));
    mgr.update(40ULL, OrderLifecycle::ACKED, 9999ULL, 0, 100000000ULL, 1000ULL);

    // now_ns = 1000 + 60s in nanoseconds, timeout = 30s
    uint64_t timeout_ns = 30ULL * 1'000'000'000ULL;
    uint64_t now = 1000ULL + 61ULL * 1'000'000'000ULL;

    std::vector<uint64_t> stale_ids;
    mgr.check_stale(now, timeout_ns, [&](const OrderState& st) { stale_ids.push_back(st.order_id); });

    EXPECT_EQ(stale_ids.size(), 1u);
    EXPECT_EQ(stale_ids[0], 40ULL);
}

TEST(OrderStateManagerTest, CheckStaleSkipsRecentOrder) {
    OrderStateManager mgr;
    mgr.insert(make_order(41ULL));
    // last_update_ns = 1000 (recent)
    mgr.update(41ULL, OrderLifecycle::ACKED, 9999ULL, 0, 100000000ULL, 1000ULL);

    uint64_t timeout_ns = 30ULL * 1'000'000'000ULL;
    uint64_t now = 1000ULL + 10ULL * 1'000'000'000ULL;  // only 10s later

    std::vector<uint64_t> stale_ids;
    mgr.check_stale(now, timeout_ns, [&](const OrderState& st) { stale_ids.push_back(st.order_id); });

    EXPECT_TRUE(stale_ids.empty());
}

TEST(OrderStateManagerTest, CheckStaleSkipsPendingOrders) {
    OrderStateManager mgr;
    auto st = make_order(42ULL);
    st.last_update_ns = 1000ULL;
    mgr.insert(st);
    // Order is PENDING, not ACKED — should not be flagged as stale

    uint64_t timeout_ns = 30ULL * 1'000'000'000ULL;
    uint64_t now = 1000ULL + 61ULL * 1'000'000'000ULL;

    std::vector<uint64_t> stale_ids;
    mgr.check_stale(now, timeout_ns, [&](const OrderState& s) { stale_ids.push_back(s.order_id); });

    EXPECT_TRUE(stale_ids.empty());
}

// Unit tests for HyperliquidReconciler — phantom-fill recovery.
//
// Two surfaces under test:
//   1. The pure static `try_match` — given a candidate + fake
//      openOrders/userFills arrays, does it pick the right bucket?
//   2. The end-to-end worker thread path — does reconcile_async push
//      through the grace period and fire the terminal callback with the
//      expected match result?
//
// The worker tests use a grace period of 50 ms and time-bound waits to
// keep the suite under a second.

#include "order_gateway/adapter/hyperliquid/hyperliquid_reconciler.h"

#include <messages/OrderSide.h>
#include <messages/OrderType.h>

#include <boost/json.hpp>
#include <chrono>
#include <condition_variable>
#include <gtest/gtest.h>
#include <mutex>

namespace {

namespace json = boost::json;
using bpt::order_gateway::adapter::hyperliquid::HyperliquidReconciler;
using MK = HyperliquidReconciler::MatchKind;

HyperliquidReconciler::Candidate make_candidate() {
    return HyperliquidReconciler::Candidate{
        /*client_order_id*/ 42,
        /*instrument_id*/ 1,
        /*side*/ bpt::messages::OrderSide::BUY,
        /*order_type*/ bpt::messages::OrderType::LIMIT,
        /*price_e8*/ 72198'00000000LL,  // $72198.00
        /*quantity_e8*/ 10000ULL,       // 0.0001 BTC
        /*exchange_symbol*/ "BTC",
        /*sent_ns*/ 1'700'000'000'000'000'000ULL,  // 1700000000000ms
    };
}

json::object open_order(const char* coin, const char* side, const char* sz, const char* px, uint64_t oid) {
    json::object o;
    o["coin"] = coin;
    o["side"] = side;
    o["sz"] = sz;
    o["limitPx"] = px;
    o["oid"] = static_cast<int64_t>(oid);
    return o;
}

json::object user_fill(const char* coin,
                       const char* side,
                       const char* sz,
                       const char* px,
                       const char* fee,
                       uint64_t oid,
                       int64_t time_ms = 1'700'000'000'000LL) {
    json::object o;
    o["coin"] = coin;
    o["side"] = side;
    o["sz"] = sz;
    o["px"] = px;
    o["fee"] = fee;
    o["oid"] = static_cast<int64_t>(oid);
    o["time"] = time_ms;
    return o;
}

constexpr int64_t kTick = 100'000'000LL;  // $1

// ---------------------------------------------------------------------------
// try_match — pure logic
// ---------------------------------------------------------------------------

TEST(HLReconcilerMatch, NoMatchWhenArraysEmpty) {
    auto r = HyperliquidReconciler::try_match(make_candidate(), {}, {}, kTick);
    EXPECT_EQ(r.kind, MK::None);
}

TEST(HLReconcilerMatch, MatchesRestingOpenOrder) {
    json::array opens;
    opens.push_back(open_order("BTC", "B", "0.0001", "72198.0", 999));
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, {}, kTick);
    EXPECT_EQ(r.kind, MK::OpenOrder);
    EXPECT_EQ(r.exch_oid, 999u);
}

TEST(HLReconcilerMatch, IgnoresDifferentCoin) {
    json::array opens;
    opens.push_back(open_order("ETH", "B", "0.0001", "72198.0", 999));
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, {}, kTick);
    EXPECT_EQ(r.kind, MK::None);
}

TEST(HLReconcilerMatch, IgnoresOppositeSide) {
    json::array opens;
    opens.push_back(open_order("BTC", "A", "0.0001", "72198.0", 999));
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, {}, kTick);
    EXPECT_EQ(r.kind, MK::None);
}

TEST(HLReconcilerMatch, IgnoresDifferentQuantity) {
    json::array opens;
    opens.push_back(open_order("BTC", "B", "0.0002", "72198.0", 999));
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, {}, kTick);
    EXPECT_EQ(r.kind, MK::None);
}

TEST(HLReconcilerMatch, AcceptsPriceWithinTick) {
    json::array opens;
    opens.push_back(open_order("BTC", "B", "0.0001", "72199.0", 999));  // $1 over
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, {}, kTick);
    EXPECT_EQ(r.kind, MK::OpenOrder);
    EXPECT_EQ(r.exch_oid, 999u);
}

TEST(HLReconcilerMatch, RejectsPriceBeyondTick) {
    json::array opens;
    opens.push_back(open_order("BTC", "B", "0.0001", "72200.0", 999));  // $2 over
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, {}, kTick);
    EXPECT_EQ(r.kind, MK::None);
}

TEST(HLReconcilerMatch, PrefersUserFillOverOpenOrder) {
    // If BOTH a resting order and a fill match, return the fill — more
    // specific terminal state (the resting order is about to disappear).
    json::array opens;
    opens.push_back(open_order("BTC", "B", "0.0001", "72198.0", 999));
    json::array fills;
    fills.push_back(user_fill("BTC", "B", "0.0001", "72198.0", "0.05", 999));
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, fills, kTick);
    EXPECT_EQ(r.kind, MK::UserFill);
    EXPECT_EQ(r.exch_oid, 999u);
    EXPECT_EQ(r.fill_qty_e8, 10000u);
}

TEST(HLReconcilerMatch, AmbiguousWhenTwoOpenOrdersMatch) {
    // Two identical-intent resting orders — can't disambiguate.
    json::array opens;
    opens.push_back(open_order("BTC", "B", "0.0001", "72198.0", 111));
    opens.push_back(open_order("BTC", "B", "0.0001", "72198.0", 222));
    auto r = HyperliquidReconciler::try_match(make_candidate(), opens, {}, kTick);
    EXPECT_EQ(r.kind, MK::Ambiguous);
}

TEST(HLReconcilerMatch, AmbiguousWhenTwoFillsMatch) {
    json::array fills;
    fills.push_back(user_fill("BTC", "B", "0.0001", "72198.0", "0.05", 111));
    fills.push_back(user_fill("BTC", "B", "0.0001", "72198.0", "0.05", 222));
    auto r = HyperliquidReconciler::try_match(make_candidate(), {}, fills, kTick);
    EXPECT_EQ(r.kind, MK::Ambiguous);
}

TEST(HLReconcilerMatch, IgnoresStaleFillsBeforeSend) {
    // Fill timestamp is BEFORE our sent_ns (by more than the 1 s
    // clock-skew window) — must not be attributed to us.
    auto c = make_candidate();  // sent_ns = 1700000000000ms
    json::array fills;
    fills.push_back(user_fill("BTC",
                              "B",
                              "0.0001",
                              "72198.0",
                              "0.05",
                              999,
                              /*time_ms=*/1'699'999'998'000LL));  // 2s earlier
    auto r = HyperliquidReconciler::try_match(c, {}, fills, kTick);
    EXPECT_EQ(r.kind, MK::None);
}

TEST(HLReconcilerMatch, AcceptsFillWithinClockSkewWindow) {
    // Fill 500 ms before send — within the 1 s window (HL exchange
    // clock vs our wall clock), should still match.
    auto c = make_candidate();
    json::array fills;
    fills.push_back(user_fill("BTC",
                              "B",
                              "0.0001",
                              "72198.0",
                              "0.05",
                              999,
                              /*time_ms=*/1'699'999'999'500LL));
    auto r = HyperliquidReconciler::try_match(c, {}, fills, kTick);
    EXPECT_EQ(r.kind, MK::UserFill);
}

TEST(HLReconcilerMatch, PartialFillReturnsActualQty) {
    // IOC that partially filled: HL's userFills entry has sz < qty.
    auto c = make_candidate();
    json::array fills;
    fills.push_back(user_fill("BTC", "B", "0.00005", "72198.0", "0.025", 999));
    auto r = HyperliquidReconciler::try_match(c, {}, fills, kTick);
    EXPECT_EQ(r.kind, MK::UserFill);
    EXPECT_EQ(r.fill_qty_e8, 5000u);  // 0.00005 = 5000e-8
}

// ---------------------------------------------------------------------------
// worker_loop — end-to-end reconcile_async
// ---------------------------------------------------------------------------

class HLReconcilerWorkerFixture : public ::testing::Test {
protected:
    std::mutex mu;
    std::condition_variable cv;
    bool terminal_fired{false};
    HyperliquidReconciler::Candidate captured_c{};
    HyperliquidReconciler::MatchResult captured_r{};

    HyperliquidReconciler::OnTerminal on_terminal() {
        return [this](const HyperliquidReconciler::Candidate& c, const HyperliquidReconciler::MatchResult& r) {
            std::lock_guard<std::mutex> lock(mu);
            captured_c = c;
            captured_r = r;
            terminal_fired = true;
            cv.notify_one();
        };
    }

    bool wait_for_terminal(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, timeout, [this] { return terminal_fired; });
    }
};

TEST_F(HLReconcilerWorkerFixture, FiresRejectedWhenPollerReturnsEmpty) {
    HyperliquidReconciler reconciler([] { return std::make_pair(json::array{}, json::array{}); },
                                     on_terminal(),
                                     std::chrono::milliseconds(50),
                                     kTick);
    reconciler.reconcile_async(make_candidate());
    ASSERT_TRUE(wait_for_terminal(std::chrono::seconds(2)));
    EXPECT_EQ(captured_r.kind, MK::None);
    EXPECT_EQ(captured_c.client_order_id, 42u);
}

TEST_F(HLReconcilerWorkerFixture, FiresAckWhenOpenOrdersHasMatch) {
    HyperliquidReconciler reconciler(
        [] {
            json::array opens;
            opens.push_back(open_order("BTC", "B", "0.0001", "72198.0", 777));
            return std::make_pair(opens, json::array{});
        },
        on_terminal(),
        std::chrono::milliseconds(50),
        kTick);
    reconciler.reconcile_async(make_candidate());
    ASSERT_TRUE(wait_for_terminal(std::chrono::seconds(2)));
    EXPECT_EQ(captured_r.kind, MK::OpenOrder);
    EXPECT_EQ(captured_r.exch_oid, 777u);
}

TEST_F(HLReconcilerWorkerFixture, FiresFillWhenUserFillsHasMatch) {
    HyperliquidReconciler reconciler(
        [] {
            json::array fills;
            fills.push_back(user_fill("BTC", "B", "0.0001", "72198.0", "0.05", 888));
            return std::make_pair(json::array{}, fills);
        },
        on_terminal(),
        std::chrono::milliseconds(50),
        kTick);
    reconciler.reconcile_async(make_candidate());
    ASSERT_TRUE(wait_for_terminal(std::chrono::seconds(2)));
    EXPECT_EQ(captured_r.kind, MK::UserFill);
    EXPECT_EQ(captured_r.exch_oid, 888u);
    EXPECT_EQ(captured_r.fill_qty_e8, 10000u);
}

TEST_F(HLReconcilerWorkerFixture, RejectedOnPollerException) {
    // Poller throws (e.g. REST timeout) — reconciler swallows and
    // falls through to the "no match" path rather than leaving the
    // candidate in limbo forever.
    HyperliquidReconciler reconciler(
        []() -> std::pair<json::array, json::array> { throw std::runtime_error("simulated REST failure"); },
        on_terminal(),
        std::chrono::milliseconds(50),
        kTick);
    reconciler.reconcile_async(make_candidate());
    ASSERT_TRUE(wait_for_terminal(std::chrono::seconds(2)));
    EXPECT_EQ(captured_r.kind, MK::None);
}

}  // namespace

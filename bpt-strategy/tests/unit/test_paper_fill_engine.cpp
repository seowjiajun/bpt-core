#include "strategy/order/paper_fill_engine.h"

#include <gtest/gtest.h>
#include <vector>

namespace {

using bpt::strategy::order::PaperFillEngine;
using bpt::strategy::order::PaperFillEvent;
using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectReason;
using bpt::messages::TimeInForce;

constexpr uint64_t kInst = 42;
constexpr auto kEx = ExchangeId::OKX;

PaperFillEngine::Order make_limit(uint64_t id, OrderSide::Value side, TimeInForce::Value tif,
                                   int64_t price_e8, uint64_t qty_e8 = 1'000'000ULL) {
    PaperFillEngine::Order o;
    o.order_id = id;
    o.exchange_id = kEx;
    o.instrument_id = kInst;
    o.side = side;
    o.order_type = OrderType::LIMIT;
    o.tif = tif;
    o.price_e8 = price_e8;
    o.quantity_e8 = qty_e8;
    return o;
}

std::vector<PaperFillEvent> drain_all(PaperFillEngine& eng) {
    std::vector<PaperFillEvent> out;
    eng.drain(1000, [&](const PaperFillEvent& ev) { out.push_back(ev); });
    return out;
}

// ---------------------------------------------------------------------------
// GTC — rests + fills on trade-print sweep
// ---------------------------------------------------------------------------

TEST(PaperFillEngine, GtcBuyRestsAndFillsOnTradePrintAtOrBelow) {
    PaperFillEngine eng;
    eng.on_bbo(kInst, /*bid=*/99.0, /*ask=*/101.0, 0);
    eng.submit(make_limit(1, OrderSide::BUY, TimeInForce::GTC, 100'00000000LL), 0);

    // Expect ACKED + resting.
    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::ACKED);
    EXPECT_EQ(eng.resting_count(), 1u);

    // Trade above our bid — no fill.
    eng.on_trade(kInst, 100.5, 0.1, 1);
    EXPECT_EQ(eng.pending_count(), 0u);
    EXPECT_EQ(eng.resting_count(), 1u);

    // Trade at our bid — fills at our maker price (not the print).
    eng.on_trade(kInst, 100.0, 0.1, 2);
    evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::FILLED);
    EXPECT_EQ(evs[0].price_e8, 100'00000000LL);
    EXPECT_EQ(evs[0].filled_qty_e8, 1'000'000ULL);
    EXPECT_EQ(eng.resting_count(), 0u);
}

TEST(PaperFillEngine, GtcSellFillsWhenPrintGoesThroughOurAsk) {
    PaperFillEngine eng;
    eng.submit(make_limit(7, OrderSide::SELL, TimeInForce::GTC, 102'00000000LL), 0);
    (void)drain_all(eng);  // consume ACKED

    // Trade above our ask — sweeps us.
    eng.on_trade(kInst, 103.0, 0.05, 1);
    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::FILLED);
    EXPECT_EQ(evs[0].price_e8, 102'00000000LL);  // fills at our maker price
}

// ---------------------------------------------------------------------------
// IOC / FOK — instant cross-or-reject
// ---------------------------------------------------------------------------

TEST(PaperFillEngine, IocCrossesBboAndFillsAtOppositeBboPrice) {
    PaperFillEngine eng;
    eng.on_bbo(kInst, 99.0, 101.0, 0);
    eng.submit(make_limit(3, OrderSide::BUY, TimeInForce::IOC, 101'00000000LL), 0);

    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::FILLED);
    EXPECT_EQ(evs[0].price_e8, 101'00000000LL);  // fills at ask
    EXPECT_EQ(eng.resting_count(), 0u);
}

TEST(PaperFillEngine, IocBelowAskRejectsWithInvalidPrice) {
    PaperFillEngine eng;
    eng.on_bbo(kInst, 99.0, 101.0, 0);
    eng.submit(make_limit(4, OrderSide::BUY, TimeInForce::IOC, 100'00000000LL), 0);

    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::REJECTED);
    EXPECT_EQ(evs[0].reject_reason, RejectReason::INVALID_PRICE);
    EXPECT_EQ(eng.resting_count(), 0u);
}

TEST(PaperFillEngine, IocWithoutBboRejectsExchangeError) {
    PaperFillEngine eng;
    eng.submit(make_limit(5, OrderSide::BUY, TimeInForce::IOC, 100'00000000LL), 0);

    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::REJECTED);
    EXPECT_EQ(evs[0].reject_reason, RejectReason::EXCHANGE_ERROR);
}

// ---------------------------------------------------------------------------
// POST_ONLY — rejects on cross, rests otherwise
// ---------------------------------------------------------------------------

TEST(PaperFillEngine, PostOnlyRejectsWhenWouldCross) {
    PaperFillEngine eng;
    eng.on_bbo(kInst, 99.0, 101.0, 0);
    auto o = make_limit(6, OrderSide::BUY, TimeInForce::GTC, 101'00000000LL);
    o.order_type = OrderType::POST_ONLY;
    eng.submit(o, 0);

    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::REJECTED);
    EXPECT_EQ(evs[0].reject_reason, RejectReason::INVALID_PRICE);
}

TEST(PaperFillEngine, PostOnlyBelowAskRestsAndAcks) {
    PaperFillEngine eng;
    eng.on_bbo(kInst, 99.0, 101.0, 0);
    auto o = make_limit(8, OrderSide::BUY, TimeInForce::GTC, 100'00000000LL);
    o.order_type = OrderType::POST_ONLY;
    eng.submit(o, 0);

    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::ACKED);
    EXPECT_EQ(eng.resting_count(), 1u);
}

// ---------------------------------------------------------------------------
// Cancel
// ---------------------------------------------------------------------------

TEST(PaperFillEngine, CancelRestingOrderEmitsCancelled) {
    PaperFillEngine eng;
    eng.submit(make_limit(9, OrderSide::BUY, TimeInForce::GTC, 100'00000000LL), 0);
    (void)drain_all(eng);

    eng.cancel(9, kEx, kInst, 10);
    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::CANCELLED);
    EXPECT_EQ(eng.resting_count(), 0u);
}

TEST(PaperFillEngine, CancelUnknownOrderIsNoOp) {
    PaperFillEngine eng;
    eng.cancel(999, kEx, kInst, 0);
    EXPECT_EQ(eng.pending_count(), 0u);
}

// ---------------------------------------------------------------------------
// Isolation between instruments
// ---------------------------------------------------------------------------

TEST(PaperFillEngine, TradeOnOneInstrumentDoesNotFillOrdersOnAnother) {
    PaperFillEngine eng;
    auto a = make_limit(1, OrderSide::BUY, TimeInForce::GTC, 100'00000000LL);
    auto b = make_limit(2, OrderSide::BUY, TimeInForce::GTC, 100'00000000LL);
    b.instrument_id = kInst + 1;
    eng.submit(a, 0);
    eng.submit(b, 0);
    (void)drain_all(eng);

    eng.on_trade(kInst, 100.0, 0.1, 1);
    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].order_id, 1u);
    EXPECT_EQ(eng.resting_count(), 1u);  // the other instrument still rests
}

// ---------------------------------------------------------------------------
// MARKET — MVP rejects it
// ---------------------------------------------------------------------------

TEST(PaperFillEngine, MarketOrderRejected) {
    PaperFillEngine eng;
    eng.on_bbo(kInst, 99.0, 101.0, 0);
    auto o = make_limit(10, OrderSide::BUY, TimeInForce::IOC, 0);
    o.order_type = OrderType::MARKET;
    eng.submit(o, 0);

    auto evs = drain_all(eng);
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(evs[0].status, ExecStatus::REJECTED);
    EXPECT_EQ(evs[0].reject_reason, RejectReason::RISK_REJECTED);
}

}  // namespace

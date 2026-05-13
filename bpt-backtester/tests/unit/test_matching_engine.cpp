// Unit tests for bpt::backtester::matching::MatchingEngine
#include "backtester/data/market_event.h"
#include "backtester/data/orderbook_record.h"
#include "backtester/data/trade_record.h"
#include "backtester/matching/matching_engine.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace bpt::backtester::matching;
using namespace bpt::backtester::data;

// ── Helpers ───────────────────────────────────────────────────────────────────

static MarketEvent make_book(const std::string& exchange,
                             const std::string& symbol,
                             double bid,
                             double ask,
                             double size = 10.0,
                             uint64_t ts = 1000) {
    OrderBookRecord ob;
    ob.timestamp_ns = ts;
    ob.exchange = exchange;
    ob.symbol = symbol;
    for (int i = 0; i < kOrderBookDepth; ++i) {
        ob.bid_px[i] = bid - i * 0.01;
        ob.bid_sz[i] = size;
        ob.ask_px[i] = ask + i * 0.01;
        ob.ask_sz[i] = size;
    }
    return MarketEvent::from_orderbook(ob);
}

static OpenOrder make_order(OrderType type,
                            OrderSide side,
                            double qty,
                            double price = 0.0,
                            const std::string& oid = "ord1",
                            const std::string& coid = "client1") {
    OpenOrder o;
    o.order_id = oid;
    o.client_order_id = coid;
    o.exchange = "BINANCE";
    o.symbol = "BTCUSDT";
    o.type = type;
    o.side = side;
    o.quantity = qty;
    o.price = price;
    return o;
}

static MarketEvent make_trade(const std::string& exchange,
                              const std::string& symbol,
                              TradeSide side,
                              double price,
                              double qty,
                              uint64_t ts = 2000) {
    TradeRecord t;
    t.timestamp_ns = ts;
    t.exchange = exchange;
    t.symbol = symbol;
    t.side = side;
    t.price = price;
    t.quantity = qty;
    return MarketEvent::from_trade(t);
}

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(MatchingEngineTest, MarketBuyFillsAtBestAsk) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));

    auto order = make_order(OrderType::MARKET, OrderSide::BUY, 3.0);
    auto result = eng.submit_order(order);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 3.0);
    EXPECT_DOUBLE_EQ(fills[0].cumulative_fill_qty, 3.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
    // Phase 3: submit_order returns pre-match state (ACCEPTED). Post-fill
    // state lives in the FillReport delivered via fill_cb above.
    EXPECT_FALSE(result.rejected);
}

TEST(MatchingEngineTest, MarketSellFillsAtBestBid) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));

    auto result = eng.submit_order(make_order(OrderType::MARKET, OrderSide::SELL, 2.0));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 100.0);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 2.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
}

TEST(MatchingEngineTest, MarketOrderWalksMultipleLevels) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Each level has size 2.0; best ask=101.0, next=101.01, ...
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 2.0));

    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 5.0));

    // 2 fills: 2@101.0 + 2@101.01 + 1@101.02 = 3 levels
    ASSERT_EQ(fills.size(), 3u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_DOUBLE_EQ(fills[1].last_fill_price, 101.01);
    EXPECT_DOUBLE_EQ(fills[2].last_fill_price, 101.02);
    EXPECT_TRUE(fills.back().is_fully_filled);
}

TEST(MatchingEngineTest, LimitBuyFillsWhenTradePrintsAtOurPrice) {
    // Queue-aware model: LIMIT fills happen when a trade prints at our
    // price (or better-for-us), not just when the BBO moves. This
    // mirrors live exchange semantics: the BBO showing your price
    // doesn't mean YOU got filled — only an explicit print does.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Book: bid=101, ask=102. Our LIMIT BUY at 101 joins the existing
    // bid level (queue_ahead = 10 from the seeded snapshot).
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 101.0, 102.0, 10.0));

    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 101.0));
    EXPECT_EQ(fills.size(), 0u);

    // Counterparty sells 12 units at 101. Drains 10 of queue_ahead, then
    // fills our 1 unit (residual 2 unused).
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 101.0, 12.0));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 1.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
}

TEST(MatchingEngineTest, LimitSellFillsWhenTradePrintsAtOurPrice) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Book: bid=98, ask=99. Our LIMIT SELL at 99 joins the existing
    // ask level (queue_ahead = 10).
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 98.0, 99.0, 10.0));

    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 1.0, 99.0));
    EXPECT_EQ(fills.size(), 0u);

    // Counterparty buys 12 at 99. Drains queue then fills us.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 99.0, 12.0));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 99.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
}

// ── Queue-aware behavior ─────────────────────────────────────────────────────

TEST(MatchingEngineTest, OrderBehindQueueDoesNotFillIfPrintTooSmall) {
    // queue_ahead = 5. A 3-unit print drains queue but doesn't reach us.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 100.0));

    // Print of 3 < queue_ahead of 5 → no fill, queue drains to 2.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 100.0, 3.0));
    EXPECT_EQ(fills.size(), 0u);

    // Subsequent print of 3 — drains remaining 2 of queue + fills us 1.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 100.0, 3.0));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 1.0);
}

TEST(MatchingEngineTest, OrderAtNewLevelHasNoQueueAhead) {
    // Book bid=100/ask=102. A LIMIT BUY at 101 joins a NEW level
    // between bid and ask — queue_ahead = 0 (we're alone).
    // First print at 101 fills us immediately (no queue to drain).
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 102.0, 10.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 101.0));

    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 101.0, 1.0));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
}

TEST(MatchingEngineTest, TradeAboveLimitDoesNotFillBuy) {
    // BUY @ 100, trade prints at 101 (above our limit) → not eligible.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 100.0));

    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 101.0, 10.0));
    EXPECT_EQ(fills.size(), 0u);
}

TEST(MatchingEngineTest, BuyTradeDoesNotFillBuyOrder) {
    // BUY orders only fill on SELL-side trades (taker sold into bids).
    // A BUY-side trade (taker bought from asks) shouldn't touch resting BUYs.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 100.0));

    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 100.0, 100.0));
    EXPECT_EQ(fills.size(), 0u);
}

TEST(MatchingEngineTest, MakerLimitFillIsLabeledMaker) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 100.0));
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 100.0, 10.0));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].liquidity_role, LiquidityRole::MAKER);
}

TEST(MatchingEngineTest, MarketOrderIsLabeledTaker) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));
    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 1.0));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].liquidity_role, LiquidityRole::TAKER);
}

// ── Crossing-LIMIT TAKER fills ──────────────────────────────────────────────

TEST(MatchingEngineTest, CrossingLimitBuyFillsAsTakerAtTouch) {
    // Book ask=101, our LIMIT BUY @ 102 crosses → fill at 101 (the
    // ask, NOT our limit), tagged TAKER.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 102.0));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_EQ(fills[0].liquidity_role, LiquidityRole::TAKER);
    EXPECT_EQ(fills[0].order_type, OrderType::LIMIT);
}

TEST(MatchingEngineTest, CrossingLimitSellFillsAsTakerAtTouch) {
    // Book bid=99, LIMIT SELL @ 98 crosses → fill at 99, TAKER.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 99.0, 100.0, 5.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 1.0, 98.0));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 99.0);
    EXPECT_EQ(fills[0].liquidity_role, LiquidityRole::TAKER);
}

TEST(MatchingEngineTest, CrossingLimitWalksMultipleLevelsAtCappedPrice) {
    // Book asks: 101 sz=2, 101.01 sz=2, 101.02 sz=2.
    // LIMIT BUY 5 @ 101.01 → walks 2@101 + 2@101.01, stops (next ask
    // 101.02 > limit). Filled 4, residual 1 rests at 101.01.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 2.0));

    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 5.0, 101.01));

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_price, 101.0);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 2.0);
    EXPECT_EQ(fills[0].liquidity_role, LiquidityRole::TAKER);
    EXPECT_DOUBLE_EQ(fills[1].last_fill_price, 101.01);
    EXPECT_DOUBLE_EQ(fills[1].last_fill_qty, 2.0);
    EXPECT_EQ(fills[1].liquidity_role, LiquidityRole::TAKER);
    EXPECT_FALSE(fills[1].is_fully_filled);
}

TEST(MatchingEngineTest, NonCrossingLimitDoesNotFillAtSubmit) {
    // Standard maker: BUY @ 100.5 with ask at 101 doesn't cross.
    // Should rest in pending without firing TAKER fill at submit.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 100.5));

    EXPECT_EQ(fills.size(), 0u);
}

TEST(MatchingEngineTest, CrossingLimitResidualRestsAsMaker) {
    // Cross fills 2 of 5 at touch (TAKER); residual 3 rests, then a
    // sell trade prints at our limit → maker fill on the residual.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Book has only 2 units at the ask, deeper levels too high.
    OrderBookRecord ob;
    ob.timestamp_ns = 1000;
    ob.exchange = "BINANCE";
    ob.symbol = "BTCUSDT";
    ob.bid_px[0] = 99.0;
    ob.bid_sz[0] = 10.0;
    ob.ask_px[0] = 100.0;
    ob.ask_sz[0] = 2.0;
    ob.ask_px[1] = 102.0;
    ob.ask_sz[1] = 10.0;  // gap; out of cross range
    eng.on_market_event(MarketEvent::from_orderbook(ob));

    // BUY 5 @ 100 crosses → fills 2 at 100 (TAKER), 3 residual rests at 100.
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 5.0, 100.0));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].liquidity_role, LiquidityRole::TAKER);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 2.0);

    // SELL trade prints 5 at 100 → drains queue (none ahead of us at
    // our new level → queue_ahead seeded as bid_qty at price 100, which
    // is 0 since 100 wasn't an existing bid level), then fills our 3.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 100.0, 5.0));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[1].liquidity_role, LiquidityRole::MAKER);
    EXPECT_DOUBLE_EQ(fills[1].last_fill_qty, 3.0);
    EXPECT_DOUBLE_EQ(fills[1].last_fill_price, 100.0);  // limit price, not trade price
}

TEST(MatchingEngineTest, LimitOrderDoesNotFillAboveLimit) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 105.0));

    // Limit buy at 101, but ask is 105 — should not fill
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 101.0));

    // Tick with ask still above limit
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 101.0, 103.0, 10.0, 2000));
    EXPECT_EQ(fills.size(), 0u);
}

TEST(MatchingEngineTest, CancelRemovesOrder) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 105.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 1.0, 101.0, "ord-cancel"));

    EXPECT_TRUE(eng.cancel_order("BINANCE", "BTCUSDT", "ord-cancel"));
    EXPECT_FALSE(eng.cancel_order("BINANCE", "BTCUSDT", "ord-cancel"));  // already gone

    // Price crosses — should not fire fill
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.5, 101.0, 10.0, 2000));
    EXPECT_EQ(fills.size(), 0u);
}

TEST(MatchingEngineTest, MarketOrderWithNoBookDoesNotCrash) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // No book update — market order arrives
    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 1.0));
    EXPECT_EQ(fills.size(), 0u);  // no book → no fill, just a warning logged
}

TEST(MatchingEngineTest, FillReportFieldsAreCorrect) {
    MatchingEngine eng;
    FillReport captured;
    eng.set_fill_callback([&](FillReport r) { captured = r; });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.5, 10.0, 5000));

    OpenOrder o = make_order(OrderType::MARKET, OrderSide::BUY, 2.0, 0.0, "myOrd", "myCid");
    eng.submit_order(o);

    EXPECT_EQ(captured.order_id, "myOrd");
    EXPECT_EQ(captured.client_order_id, "myCid");
    EXPECT_EQ(captured.symbol, "BTCUSDT");
    EXPECT_EQ(captured.exchange, "BINANCE");
    EXPECT_EQ(captured.side, OrderSide::BUY);
    EXPECT_DOUBLE_EQ(captured.original_qty, 2.0);
    EXPECT_DOUBLE_EQ(captured.last_fill_price, 101.5);
    EXPECT_EQ(captured.simulation_ts, 5000u);
}

// ── Deferred match (Phase 3 / Option A) ──────────────────────────────────────

#include "backtester/latency/latency_model.h"

using bpt::backtester::latency::LatencyLeg;
using bpt::backtester::latency::ParametricLatencyModel;

TEST(MatchingEngineLatencyTest, MarketOrderDoesNotFillUntilSubmitToMatchElapses) {
    MatchingEngine eng;
    ParametricLatencyModel m(/*seed=*/1);
    m.set_spec("BINANCE", LatencyLeg::SUBMIT_TO_MATCH, {/*base=*/50'000'000ULL, 0});  // 50ms
    eng.set_latency_model(&m);

    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, /*ts=*/1'000'000'000ULL));
    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 3.0));
    EXPECT_TRUE(fills.empty()) << "submit_order must not fill synchronously when latency is non-zero";

    // Event 30ms later — still inside the latency window.
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, 1'030'000'000ULL));
    EXPECT_TRUE(fills.empty());

    // Event 60ms after submit — past the 50ms scheduled_match_ts.
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, 1'060'000'000ULL));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 3.0);
}

TEST(MatchingEngineLatencyTest, FillDeliveryDelayedByMatchToReport) {
    MatchingEngine eng;
    ParametricLatencyModel m(/*seed=*/1);
    m.set_spec("BINANCE", LatencyLeg::MATCH_TO_REPORT, {/*base=*/20'000'000ULL, 0});  // 20ms
    eng.set_latency_model(&m);

    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, 1'000'000'000ULL));

    // SUBMIT_TO_MATCH defaults to 0 → match runs at submit time, but
    // delivery is delayed by 20ms.
    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 1.0));
    EXPECT_TRUE(fills.empty()) << "fill_cb must wait for match_to_report delivery latency";

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, 1'010'000'000ULL));
    EXPECT_TRUE(fills.empty()) << "10ms < 20ms — still in delivery window";

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, 1'025'000'000ULL));
    ASSERT_EQ(fills.size(), 1u);
}

TEST(MatchingEngineLatencyTest, CancelDuringSubmitToMatchPreventsFill) {
    MatchingEngine eng;
    ParametricLatencyModel m(/*seed=*/1);
    m.set_spec("BINANCE", LatencyLeg::SUBMIT_TO_MATCH, {/*base=*/100'000'000ULL, 0});  // 100ms
    eng.set_latency_model(&m);

    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, 1'000'000'000ULL));
    eng.submit_order(make_order(OrderType::MARKET, OrderSide::BUY, 1.0, /*price=*/0.0, "ord-1"));

    // Cancel before scheduled_match_ts.
    EXPECT_TRUE(eng.cancel_order("BINANCE", "BTCUSDT", "ord-1"));

    // Time passes well beyond the latency window — no fill should fire.
    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 5.0, 2'000'000'000ULL));
    EXPECT_TRUE(fills.empty());
}

// ── Queue regen on cancels-ahead (Phase 5) ───────────────────────────────────

// Build a book where level 1 sizes can be set independently. Other levels
// follow the make_book pattern so unrelated tests work.
static MarketEvent make_book_l1(const std::string& exchange,
                                const std::string& symbol,
                                double bid,
                                double ask,
                                double bid_sz_l1,
                                double ask_sz_l1,
                                uint64_t ts = 1000) {
    OrderBookRecord ob;
    ob.timestamp_ns = ts;
    ob.exchange = exchange;
    ob.symbol = symbol;
    for (int i = 0; i < kOrderBookDepth; ++i) {
        ob.bid_px[i] = bid - i * 0.01;
        ob.bid_sz[i] = (i == 0) ? bid_sz_l1 : 10.0;
        ob.ask_px[i] = ask + i * 0.01;
        ob.ask_sz[i] = (i == 0) ? ask_sz_l1 : 10.0;
    }
    return MarketEvent::from_orderbook(ob);
}

// Inspect queue_ahead by attempting fills that depend on it. We use a
// tiny print and check whether our resting order fills (queue_ahead too
// large) or gets eaten (queue_ahead small enough). The test expectations
// are stated in terms of trade-volume thresholds for clarity.
TEST(MatchingEngineQueueRegenTest, AttributesPureCancellationsToQueueAhead) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Initial book: ask level 1 has size 100 @ 101.0.
    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, /*bid_sz1=*/10, /*ask_sz1=*/100));

    // Resting SELL at 101 with qty 5. queue_ahead seeds to 100.
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 5.0, 101.0, "sell-1"));
    EXPECT_EQ(fills.size(), 0u);

    // Second book: ask size at 101.0 drops to 60. No trades intervened.
    // 40 units of cancellation → queue_ahead expected = 100 - 100*(40/100) = 60.
    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 60, /*ts=*/2000));

    // Sanity-check via a trade print: a 50-unit BUY print at 101 should
    // drain the (regenerated) queue_ahead=60 partially without filling us.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 50.0, 2500));
    EXPECT_EQ(fills.size(), 0u) << "queue_ahead 60 > 50 print should leave us unfilled";
    // After this print: queue_ahead = 10.

    // A second 15-unit print clears the residual queue (10) and fully
    // consumes our 5-unit order. With no regen, cumulative 105 prints
    // would be needed before our order fills (queue_ahead would still
    // be 100 after the book "redrew" to 60).
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 15.0, 3000));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 5.0);
}

TEST(MatchingEngineQueueRegenTest, SubtractsTradeVolumeBeforeAttribution) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 100));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 5.0, 101.0, "sell-1"));

    // 30 units traded at 101 between the two book updates.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 30.0, 1500));
    // queue_ahead now reduced by 30 from fill_against_trade → 70.

    // Book drops to 50 (Δ=50). Trade-attributable=30 → cancels=20.
    // End-weighted attribution: pos_frac = 70/100 = 0.7,
    // cancel_share = 20 * 0.7² = 20 * 0.49 = 9.8.
    // queue_ahead expected = 70 - 9.8 = 60.2.
    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 50, /*ts=*/2000));

    // A 60-unit print at 101 leaves us unfilled — queue_ahead 60.2 still covers.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 60.0, 2500));
    EXPECT_EQ(fills.size(), 0u);

    // A 5-unit print drains the residual ~0.2 queue and partially fills us.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 5.0, 3000));
    ASSERT_EQ(fills.size(), 1u);
}

TEST(MatchingEngineQueueRegenTest, NoRegenWhenLevelGrows) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 50));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 5.0, 101.0, "sell-1"));

    // Book grows to 80 — level got more volume. queue_ahead should stay 50.
    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 80, /*ts=*/2000));

    // 50-unit print just covers the original queue_ahead; does NOT fill us.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 50.0, 2500));
    EXPECT_EQ(fills.size(), 0u);

    // 1 more unit pushes through.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 1.0, 3000));
    ASSERT_EQ(fills.size(), 1u);
}

TEST(MatchingEngineQueueRegenTest, NoRegenWhenLevelDropsOffVisibleBook) {
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 100));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 5.0, 101.0, "sell-1"));

    // Spread widens — best ask is now 102.0. Our 101 is no longer at L1
    // and the lookup at our price returns 0. queue_ahead must be left alone.
    OrderBookRecord narrow;
    narrow.timestamp_ns = 2000;
    narrow.exchange = "BINANCE";
    narrow.symbol = "BTCUSDT";
    for (int i = 0; i < kOrderBookDepth; ++i) {
        narrow.bid_px[i] = 100.0 - i * 0.01;
        narrow.bid_sz[i] = 10.0;
        narrow.ask_px[i] = 102.0 + i * 0.01;  // 101 is gone
        narrow.ask_sz[i] = 10.0;
    }
    eng.on_market_event(MarketEvent::from_orderbook(narrow));

    // Trade prints at 101 are no longer in the book, but if any happen,
    // they consume queue_ahead via fill_against_trade as before. Our
    // queue_ahead is *still* 100 (regen skipped the disappearance).
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 99.0, 2500));
    EXPECT_EQ(fills.size(), 0u) << "queue_ahead = 100 > 99 print, no fill";
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 2.0, 3000));
    EXPECT_EQ(fills.size(), 1u);
}

TEST(MatchingEngineQueueRegenTest, BackCompatSingleBookEventNoRegen) {
    // Only one book event ever, so apply_queue_regen has no prior to
    // compare against. This is the "first tick of a session" path; it
    // must not corrupt queue_ahead.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 100));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 5.0, 101.0, "sell-1"));

    // queue_ahead must still be 100 — verify by needing 100 print volume to fill.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 99.0, 1500));
    EXPECT_EQ(fills.size(), 0u);
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 2.0, 1600));
    EXPECT_EQ(fills.size(), 1u);
}

TEST(MatchingEngineLatencyTest, PostOnlyRejectionStaysSynchronous) {
    // POST_ONLY orders that would cross the *current* book are rejected
    // synchronously in the submit_order return — real exchanges return
    // this in the ack frame, so the order server's HTTP response can
    // carry the error string.
    MatchingEngine eng;
    ParametricLatencyModel m(/*seed=*/1);
    m.set_spec("HYPERLIQUID", LatencyLeg::SUBMIT_TO_MATCH, {500'000'000ULL, 0});
    eng.set_latency_model(&m);

    eng.on_market_event(make_book("HYPERLIQUID", "APE", 0.18, 0.181, 1000.0, 1'000'000'000ULL));

    // Crossing POST_ONLY buy at 0.181 (= ask) — rejects synchronously.
    auto crossing = make_order(OrderType::POST_ONLY, OrderSide::BUY, 100.0, 0.181);
    crossing.exchange = "HYPERLIQUID";
    crossing.symbol = "APE";
    auto res = eng.submit_order(crossing);
    EXPECT_TRUE(res.rejected);
}

// ── FIFO between our own resting orders at the same price ───────────────────

TEST(MatchingEngineTest, TwoOrdersSamePriceFifo_FirstFillsBeforeSecond) {
    // Two LIMIT BUYs at 101, venue size 10. First print 12 drains venue and
    // partially fills A. Second print 5 finishes A and starts filling B.
    // Third print 3 finishes B. Before this fix, B would have been stuck
    // behind a stale queue_ahead and missed fills it should have caught.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 101.0, 102.0, 10.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 5.0, 101.0, "A", "cA"));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 5.0, 101.0, "B", "cB"));
    EXPECT_EQ(fills.size(), 0u);

    // Print of 12: drains 10 venue (shared), fills A 2/5. B still has 5 of A in front.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 101.0, 12.0));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].order_id, "A");
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 2.0);

    // Print of 5: A consumes its remaining 3 (fully fills), B catches the
    // residual 2. Before the fix B would have been gated by a stale
    // queue_ahead=10 and missed this fill.
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 101.0, 5.0));
    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[1].order_id, "A");
    EXPECT_DOUBLE_EQ(fills[1].last_fill_qty, 3.0);
    EXPECT_TRUE(fills[1].is_fully_filled);
    EXPECT_EQ(fills[2].order_id, "B");
    EXPECT_DOUBLE_EQ(fills[2].last_fill_qty, 2.0);
    EXPECT_FALSE(fills[2].is_fully_filled);

    // Print of 3: finishes B (which had 3 remaining).
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 101.0, 3.0));
    ASSERT_EQ(fills.size(), 4u);
    EXPECT_EQ(fills[3].order_id, "B");
    EXPECT_DOUBLE_EQ(fills[3].last_fill_qty, 3.0);
    EXPECT_TRUE(fills[3].is_fully_filled);
}

TEST(MatchingEngineTest, SamePriceFifo_SinglePrintConsumesVenueAFillsBStarts) {
    // Single large print drains venue + walks both our orders. Tests the
    // propagation logic inside one call: A's venue drain + fill must update
    // B's counters mid-loop so B sees the right residual.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 10.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 3.0, 100.0, "A", "cA"));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 4.0, 100.0, "B", "cB"));

    // Print of 14: drains 10 venue, fills A (3), starts filling B (1).
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 100.0, 14.0));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].order_id, "A");
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 3.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
    EXPECT_EQ(fills[1].order_id, "B");
    EXPECT_DOUBLE_EQ(fills[1].last_fill_qty, 1.0);
    EXPECT_FALSE(fills[1].is_fully_filled);
}

TEST(MatchingEngineQueueRegenTest, EndWeightedFavoursFrontOfQueue) {
    // Demonstrates the key difference vs uniform attribution: an order at
    // venue position 30/100 sees materially fewer cancels in front of it
    // than an order at position 90/100 does, for the same cancel mass.
    //
    // Uniform would give the front order 30% of cancels and the back order
    // 90%. End-weighted (linear) gives the front order 9% (0.3²) and the
    // back order 81% (0.9²). The back-vs-front ratio is 9× steeper under
    // end-weighted, which is what microstructure data actually looks like.
    //
    // We assert the front-positioned order survives a print that *would*
    // have filled it under uniform but *doesn't* under end-weighted.

    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    // Front-of-queue scenario: order placed when level was 100 sees 70
    // units of venue volume drain via trade prints, leaving queue_ahead=30.
    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 100));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::SELL, 5.0, 101.0, "sell-front"));
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 70.0, 1500));
    EXPECT_EQ(fills.size(), 0u);  // not at the front yet — 30 venue still ahead

    // Now book shrinks to 22. Decrease = 100-22 = 78. Trade-attributable = 70.
    // Cancels = 8.
    //   Uniform would: queue_ahead -= 30 * (8/100) = 2.4 → queue_ahead = 27.6
    //   End-weighted: queue_ahead -= 8 * (30/100)² = 8 * 0.09 = 0.72 → queue_ahead = 29.28
    eng.on_market_event(make_book_l1("BINANCE", "BTCUSDT", 100.0, 101.0, 10, 22, /*ts=*/2000));

    // A 28-unit print at 101: under uniform attribution, queue_ahead=27.6
    // would be drained and we'd fill 0.4. Under end-weighted, queue_ahead=
    // 29.28 absorbs the whole print — no fill yet, which is the more
    // realistic outcome (the 8 cancels were mostly behind us, not ahead).
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::BUY, 101.0, 28.0, 2500));
    EXPECT_EQ(fills.size(), 0u) << "End-weighted attribution should keep this print from reaching the front order";
}

TEST(MatchingEngineTest, SamePriceFifo_CancellingAClearsBQueue) {
    // After A is cancelled, B should no longer carry A's qty in its
    // our_qty_ahead. A print large enough to fill B but not (B + A) should
    // succeed at filling B fully.
    MatchingEngine eng;
    std::vector<FillReport> fills;
    eng.set_fill_callback([&](FillReport r) { fills.push_back(r); });

    eng.on_market_event(make_book("BINANCE", "BTCUSDT", 100.0, 101.0, 10.0));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 5.0, 100.0, "A", "cA"));
    eng.submit_order(make_order(OrderType::LIMIT, OrderSide::BUY, 3.0, 100.0, "B", "cB"));

    EXPECT_TRUE(eng.cancel_order("BINANCE", "BTCUSDT", "A"));

    // Print of 13: drains 10 venue (B's queue_ahead = 0 already from cancel
    // propagation NOT touching it — venue is shared so it still sees 10),
    // then fills B fully (3).
    eng.on_market_event(make_trade("BINANCE", "BTCUSDT", TradeSide::SELL, 100.0, 13.0));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].order_id, "B");
    EXPECT_DOUBLE_EQ(fills[0].last_fill_qty, 3.0);
    EXPECT_TRUE(fills[0].is_fully_filled);
}

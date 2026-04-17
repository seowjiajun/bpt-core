#include "md_gateway/md/md_validator.h"

#include <messages/TradeSide.h>

#include <gtest/gtest.h>

namespace bpt::md_gateway::md {
namespace {

using VS = bpt::messages::TradeSide;

// Helper: a valid BBO at price ~30000
MdBbo make_bbo(uint64_t id = 1, double bid = 29990.0, double ask = 29991.0) {
    return {0, id, bid, 1.0, ask, 1.0};
}

MdTrade make_trade(uint64_t id = 1, double px = 29990.5, double qty = 0.5) {
    return {0, id, px, qty, VS::BUY};
}

MdOrderBook make_book(uint64_t id = 1) {
    MdOrderBook b;
    b.instrument_id = id;
    b.bids = {{29990.0, 1.0}, {29989.0, 2.0}};
    b.asks = {{29991.0, 1.0}, {29992.0, 2.0}};
    return b;
}

// ── BBO ───────────────────────────────────────────────────────────────────────

TEST(MdValidatorTest, BboValidPassesThrough) {
    MdValidator v;
    EXPECT_EQ(v.validate(make_bbo()), ValidationResult::OK);
}

TEST(MdValidatorTest, BboZeroBidPriceDropped) {
    MdValidator v;
    auto bbo = make_bbo();
    bbo.bid_price = 0.0;
    EXPECT_EQ(v.validate(bbo), ValidationResult::DROP);
}

TEST(MdValidatorTest, BboZeroAskQtyDropped) {
    MdValidator v;
    auto bbo = make_bbo();
    bbo.ask_qty = 0.0;
    EXPECT_EQ(v.validate(bbo), ValidationResult::DROP);
}

TEST(MdValidatorTest, BboCrossedDropped) {
    MdValidator v;
    EXPECT_EQ(v.validate(make_bbo(1, 30000.0, 29999.0)), ValidationResult::DROP);
}

TEST(MdValidatorTest, BboEqualBidAskDropped) {
    MdValidator v;
    EXPECT_EQ(v.validate(make_bbo(1, 30000.0, 30000.0)), ValidationResult::DROP);
}

TEST(MdValidatorTest, BboFirstTickAlwaysPasses) {
    MdValidator v;
    // No prior mid — first tick accepted regardless of value.
    EXPECT_EQ(v.validate(make_bbo(1, 100.0, 101.0)), ValidationResult::OK);
}

TEST(MdValidatorTest, BboSmallDeviationPasses) {
    MdValidator v(10.0);
    v.validate(make_bbo(1, 29990.0, 29991.0));  // seed mid ~29990.5
    // 5% move — within 10% threshold
    EXPECT_EQ(v.validate(make_bbo(1, 31489.0, 31490.0)), ValidationResult::OK);
}

TEST(MdValidatorTest, BboLargeDeviationDropped) {
    MdValidator v(10.0);
    v.validate(make_bbo(1, 29990.0, 29991.0));  // seed mid ~29990.5
    // >10% move
    EXPECT_EQ(v.validate(make_bbo(1, 35000.0, 35001.0)), ValidationResult::DROP);
}

TEST(MdValidatorTest, BboDeviationIndependentPerInstrument) {
    MdValidator v(10.0);
    v.validate(make_bbo(1, 29990.0, 29991.0));
    v.validate(make_bbo(2, 1000.0, 1001.0));
    // Large move on instrument 1 should not affect instrument 2.
    EXPECT_EQ(v.validate(make_bbo(1, 35000.0, 35001.0)), ValidationResult::DROP);
    EXPECT_EQ(v.validate(make_bbo(2, 1050.0, 1051.0)), ValidationResult::OK);
}

TEST(MdValidatorTest, BboLastMidUpdatedOnSuccess) {
    MdValidator v(10.0);
    v.validate(make_bbo(1, 29990.0, 29991.0));  // mid = 29990.5
    v.validate(make_bbo(1, 31000.0, 31001.0));  // ~3.4% move — OK, mid now ~31000.5
    // 9.7% move from new baseline — within threshold, would fail from original mid
    EXPECT_EQ(v.validate(make_bbo(1, 34000.0, 34001.0)), ValidationResult::OK);
}

TEST(MdValidatorTest, BboResetClearsMid) {
    MdValidator v(10.0);
    v.validate(make_bbo(1, 29990.0, 29991.0));
    v.reset();
    // After reset, first tick is always accepted.
    EXPECT_EQ(v.validate(make_bbo(1, 60000.0, 60001.0)), ValidationResult::OK);
}

// ── Trade ─────────────────────────────────────────────────────────────────────

TEST(MdValidatorTest, TradeValidPasses) {
    MdValidator v;
    EXPECT_EQ(v.validate(make_trade()), ValidationResult::OK);
}

TEST(MdValidatorTest, TradeZeroPriceDropped) {
    MdValidator v;
    auto t = make_trade();
    t.price = 0.0;
    EXPECT_EQ(v.validate(t), ValidationResult::DROP);
}

TEST(MdValidatorTest, TradeZeroQtyDropped) {
    MdValidator v;
    auto t = make_trade();
    t.qty = 0.0;
    EXPECT_EQ(v.validate(t), ValidationResult::DROP);
}

TEST(MdValidatorTest, TradeDeviationCheckedAgainstBboMid) {
    MdValidator v(10.0);
    v.validate(make_bbo(1, 29990.0, 29991.0));  // seeds mid ~29990.5
    // Trade at >10% from mid
    auto t = make_trade(1, 35000.0);
    EXPECT_EQ(v.validate(t), ValidationResult::DROP);
}

TEST(MdValidatorTest, TradeNoMidSkipsDeviationCheck) {
    MdValidator v(10.0);
    // No BBO seen yet — deviation check is skipped.
    EXPECT_EQ(v.validate(make_trade(1, 999999.0)), ValidationResult::OK);
}

// ── OrderBook ────────────────────────────────────────────────────────────────

TEST(MdValidatorTest, OrderBookValidPasses) {
    MdValidator v;
    EXPECT_EQ(v.validate(make_book()), ValidationResult::OK);
}

TEST(MdValidatorTest, OrderBookEmptyBidsDropped) {
    MdValidator v;
    auto b = make_book();
    b.bids.clear();
    EXPECT_EQ(v.validate(b), ValidationResult::DROP);
}

TEST(MdValidatorTest, OrderBookCrossedDropped) {
    MdValidator v;
    auto b = make_book();
    b.asks[0].first = 29989.0;  // ask < bid
    EXPECT_EQ(v.validate(b), ValidationResult::DROP);
}

TEST(MdValidatorTest, OrderBookBidsNotDescendingDropped) {
    MdValidator v;
    auto b = make_book();
    b.bids[1].first = 29991.0;  // level 1 >= level 0
    EXPECT_EQ(v.validate(b), ValidationResult::DROP);
}

TEST(MdValidatorTest, OrderBookAsksNotAscendingDropped) {
    MdValidator v;
    auto b = make_book();
    b.asks[1].first = 29990.0;  // level 1 <= level 0
    EXPECT_EQ(v.validate(b), ValidationResult::DROP);
}

TEST(MdValidatorTest, OrderBookDeviationDropped) {
    MdValidator v(10.0);
    v.validate(make_bbo(1, 29990.0, 29991.0));  // seeds mid
    auto b = make_book();
    b.bids[0].first = 35000.0;
    b.asks[0].first = 35001.0;
    EXPECT_EQ(v.validate(b), ValidationResult::DROP);
}

}  // namespace
}  // namespace bpt::md_gateway::md

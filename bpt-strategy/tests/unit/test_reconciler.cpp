#include "strategy/strategy/position_tracker.h"
#include "strategy/strategy/reconciler.h"

#include <messages/AccountSnapshot.h>
#include <messages/MessageHeader.h>

#include <array>
#include <cstring>
#include <gtest/gtest.h>

namespace {

using bpt::messages::AccountSnapshot;
using bpt::messages::ExchangeId;
using bpt::messages::MessageHeader;
using bpt::messages::OrderSide;
using bpt::strategy::strategy::extract_exchange_currency_balances;
using bpt::strategy::strategy::PositionTracker;
using bpt::strategy::strategy::reconcile;

// Encode a fresh AccountSnapshot with the given positions into buf, then
// rewrap it for decode so we can hand it to reconcile(). positions is
// {exchange_symbol, net_qty_e8}. Returns a decoded-mode AccountSnapshot
// sharing the buffer.
struct Encoded {
    std::array<char, 4096> buf{};
    AccountSnapshot msg;

    explicit Encoded(ExchangeId::Value exchange,
                     const std::vector<std::pair<std::string, int64_t>>& positions,
                     const std::vector<std::pair<std::string, int64_t>>& ccy_balances = {}) {
        AccountSnapshot writer;
        writer.wrapAndApplyHeader(buf.data(), 0, buf.size())
            .exchangeId(exchange)
            .correlationId(0)
            .timestampNs(0)
            .availableBalanceE8(0)
            .totalEquityE8(0);
        auto& group = writer.positionsCount(static_cast<uint16_t>(positions.size()));
        for (const auto& [sym, qty] : positions) {
            group.next().putExchangeSymbol(sym).netQtyE8(qty).avgEntryPriceE8(0).unrealizedPnlE8(0);
        }
        auto& ccy_group = writer.currencyBalancesCount(static_cast<uint16_t>(ccy_balances.size()));
        for (const auto& [ccy, equity] : ccy_balances) {
            ccy_group.next().putCcy(ccy).equityE8(equity).availableBalanceE8(equity);
        }
        // Rewrap for decode at the same offset.
        msg.wrapForDecode(buf.data(),
                          MessageHeader::encodedLength(),
                          AccountSnapshot::sbeBlockLength(),
                          AccountSnapshot::sbeSchemaVersion(),
                          buf.size());
    }
};

constexpr int64_t kThresh = 10000;  // 0.0001 in 1e8

TEST(ReconcilerTest, EmptyTrackerEmptyExchangeNoDivergence) {
    PositionTracker t;
    Encoded snap(ExchangeId::OKX, {});
    const std::unordered_map<uint64_t, std::string> map{};
    const auto out = reconcile(t, snap.msg, map, kThresh);
    EXPECT_TRUE(out.empty());
}

TEST(ReconcilerTest, MatchingPositionNoDivergence) {
    PositionTracker t;
    // Our tracker: long 1.0 BTC on OKX for instrument 100 (symbol "BTC-USDT").
    t.on_fill(100, ExchangeId::OKX, OrderSide::BUY, 1 * 100'000'000ULL, 50000 * 100'000'000LL);
    Encoded snap(ExchangeId::OKX, {{"BTC-USDT", 1 * 100'000'000}});
    std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, snap.msg, map, kThresh);
    EXPECT_TRUE(out.empty());
}

TEST(ReconcilerTest, DivergenceExceedsThreshold) {
    PositionTracker t;
    // We think we're long 1.0 BTC; exchange says we have 0.9 BTC.
    t.on_fill(100, ExchangeId::OKX, OrderSide::BUY, 1 * 100'000'000ULL, 50000 * 100'000'000LL);
    Encoded snap(ExchangeId::OKX, {{"BTC-USDT", 90'000'000}});  // 0.9 BTC
    std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, snap.msg, map, kThresh);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].instrument_id, 100u);
    EXPECT_EQ(out[0].exchange_symbol, "BTC-USDT");
    EXPECT_EQ(out[0].our_net_qty_e8, 100'000'000);
    EXPECT_EQ(out[0].exchange_net_qty_e8, 90'000'000);
    EXPECT_EQ(out[0].diff_e8, 10'000'000);
}

TEST(ReconcilerTest, SmallDivergenceUnderThresholdIgnored) {
    PositionTracker t;
    t.on_fill(100, ExchangeId::OKX, OrderSide::BUY, 1 * 100'000'000ULL, 50000 * 100'000'000LL);
    // Exchange reports qty 5000 e8 less (0.00005 BTC off — below 0.0001 threshold)
    Encoded snap(ExchangeId::OKX, {{"BTC-USDT", 99'995'000}});
    std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, snap.msg, map, kThresh);
    EXPECT_TRUE(out.empty());
}

TEST(ReconcilerTest, ExchangeMissingPositionReportsDivergence) {
    PositionTracker t;
    // We think we're long; exchange doesn't report it at all.
    t.on_fill(100, ExchangeId::OKX, OrderSide::BUY, 1 * 100'000'000ULL, 50000 * 100'000'000LL);
    Encoded snap(ExchangeId::OKX, {});  // exchange has no positions
    std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, snap.msg, map, kThresh);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].our_net_qty_e8, 100'000'000);
    EXPECT_EQ(out[0].exchange_net_qty_e8, 0);
    EXPECT_TRUE(out[0].exchange_symbol.empty())
        << "symbol should be empty when exchange didn't report a matching position";
}

TEST(ReconcilerTest, ExchangeExtraPositionIgnored) {
    // Exchange reports ETH holdings but we don't track ETH in this
    // strategy — should be silently ignored, not flagged as a divergence.
    PositionTracker t;
    Encoded snap(ExchangeId::OKX, {{"ETH-USDT", 2 * 100'000'000}});
    std::unordered_map<uint64_t, std::string> map{};
    const auto out = reconcile(t, snap.msg, map, kThresh);
    EXPECT_TRUE(out.empty());
}

TEST(ReconcilerTest, ShortVsLongReportsDivergence) {
    PositionTracker t;
    // We think we're short 1 BTC; exchange says long 1 BTC. Sign flip =
    // a serious bug somewhere.
    t.on_fill(100, ExchangeId::OKX, OrderSide::SELL, 1 * 100'000'000ULL, 50000 * 100'000'000LL);
    Encoded snap(ExchangeId::OKX, {{"BTC-USDT", 100'000'000}});
    std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, snap.msg, map, kThresh);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].our_net_qty_e8, -100'000'000);
    EXPECT_EQ(out[0].exchange_net_qty_e8, 100'000'000);
    EXPECT_EQ(out[0].diff_e8, -200'000'000);
}

// ── extract_exchange_currency_balances ─────────────────────────────────────
//
// Note: SBE repeating groups share a read cursor, so the positions group
// MUST be drained before currencyBalances (see header doc). Tests call
// extract_exchange_positions() first to advance the cursor, matching
// production use in AvellanedaStoikovStrategy::on_account_snapshot.

TEST(ExtractCurrencyBalancesTest, EmptyGroup) {
    using bpt::strategy::strategy::extract_exchange_positions;
    Encoded snap(ExchangeId::OKX, {});
    (void)extract_exchange_positions(snap.msg);
    const auto got = extract_exchange_currency_balances(snap.msg);
    EXPECT_TRUE(got.empty());
}

TEST(ExtractCurrencyBalancesTest, MultipleCurrencies) {
    using bpt::strategy::strategy::extract_exchange_positions;
    Encoded snap(ExchangeId::OKX,
                 {},                              // no positions
                 {{"BTC", 100'000'000},           // 1.0 BTC
                  {"USDT", 64'000'000'00000000},  // 64000 USDT
                  {"ETH", 2'000'000'000}});       // 2.0 ETH
    (void)extract_exchange_positions(snap.msg);
    const auto got = extract_exchange_currency_balances(snap.msg);
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got.at("BTC"), 100'000'000);
    EXPECT_EQ(got.at("USDT"), 64'000'000'00000000LL);
    EXPECT_EQ(got.at("ETH"), 2'000'000'000);
}

TEST(ExtractCurrencyBalancesTest, AlongsidePositions) {
    // Realistic shape — both groups populated. Positions drained first,
    // then currency balances. Mirrors what the strategy actually does.
    using bpt::strategy::strategy::extract_exchange_positions;
    Encoded snap(ExchangeId::OKX,
                 {{"BTC-USDT-SWAP", 1'000'000}},  // 0.01 contracts of a PERP
                 {{"BTC", 50'000'000}, {"USDT", 10'000'000'00000000LL}});
    const auto pos = extract_exchange_positions(snap.msg);
    const auto ccy = extract_exchange_currency_balances(snap.msg);
    ASSERT_EQ(pos.size(), 1u);
    EXPECT_EQ(pos.at("BTC-USDT-SWAP"), 1'000'000);
    ASSERT_EQ(ccy.size(), 2u);
    EXPECT_EQ(ccy.at("BTC"), 50'000'000);
    EXPECT_EQ(ccy.at("USDT"), 10'000'000'00000000LL);
}

// ── SPOT reconcile pattern (delta against session-start baseline) ──────────
//
// These tests exercise the caller-side logic that the strategy runs: for
// SPOT entries we pass reconcile() a synthetic exchange_by_symbol map
// where the symbol's "net qty" is current_ccy_equity - initial_ccy_equity.
// The reconciler itself doesn't know about SPOT/PERP — it just compares.

TEST(SpotReconcileTest, BaselineMinusCurrentEqualsTrackerNoDivergence) {
    // Session opens with 10 BTC holding. Strategy buys 0.5 BTC — tracker
    // records +0.5, exchange now reports 10.5 BTC. Delta = +0.5 matches
    // tracker; no divergence should fire.
    PositionTracker t;
    t.on_fill(100, ExchangeId::OKX, OrderSide::BUY, 50'000'000ULL, 50000 * 100'000'000LL);  // +0.5 BTC
    const int64_t initial_btc = 10 * 100'000'000LL;
    const int64_t current_btc = initial_btc + 50'000'000;  // +0.5 BTC
    const int64_t delta = current_btc - initial_btc;
    const std::unordered_map<std::string, int64_t> exchange_view{{"BTC-USDT", delta}};
    const std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, exchange_view, ExchangeId::OKX, map, kThresh);
    EXPECT_TRUE(out.empty());
}

TEST(SpotReconcileTest, DeltaDoesNotMatchTrackerReportsDivergence) {
    // Strategy thinks it bought 0.5 BTC; exchange balance only moved
    // +0.4 BTC (a fill slipped somewhere). Should flag divergence.
    PositionTracker t;
    t.on_fill(100, ExchangeId::OKX, OrderSide::BUY, 50'000'000ULL, 50000 * 100'000'000LL);
    const int64_t initial_btc = 10 * 100'000'000LL;
    const int64_t current_btc = initial_btc + 40'000'000;  // only +0.4 BTC
    const int64_t delta = current_btc - initial_btc;
    const std::unordered_map<std::string, int64_t> exchange_view{{"BTC-USDT", delta}};
    const std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, exchange_view, ExchangeId::OKX, map, kThresh);
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].our_net_qty_e8, 50'000'000);
    EXPECT_EQ(out[0].exchange_net_qty_e8, 40'000'000);
    EXPECT_EQ(out[0].diff_e8, 10'000'000);
}

TEST(SpotReconcileTest, SellMovesBalanceDownMatchesNegativeTracker) {
    // Session opens with 10 BTC. Strategy sells 0.3 BTC — tracker -0.3,
    // exchange now at 9.7 BTC. Delta = -0.3 matches tracker.
    PositionTracker t;
    t.on_fill(100, ExchangeId::OKX, OrderSide::SELL, 30'000'000ULL, 50000 * 100'000'000LL);
    const int64_t initial_btc = 10 * 100'000'000LL;
    const int64_t current_btc = initial_btc - 30'000'000;
    const int64_t delta = current_btc - initial_btc;
    const std::unordered_map<std::string, int64_t> exchange_view{{"BTC-USDT", delta}};
    const std::unordered_map<uint64_t, std::string> map{{100, "BTC-USDT"}};
    const auto out = reconcile(t, exchange_view, ExchangeId::OKX, map, kThresh);
    EXPECT_TRUE(out.empty());
}

}  // namespace

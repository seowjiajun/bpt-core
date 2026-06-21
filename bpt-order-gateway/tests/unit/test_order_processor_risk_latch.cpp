// Integration-style unit test for OrderProcessor's daily-loss kill
// switch. Feeds synthetic ExecEvents through on_exec_event and asserts:
//   1. While cumulative realized P&L is above -max_daily_loss_usd,
//      trading stays enabled and nothing is latched.
//   2. The fill that pushes us below the threshold flips the kill
//      switch and leaves it latched.
//   3. Subsequent NewOrder attempts reject via RISK_REJECTED at the
//      pretrade gate.
//
// Closes the verification gap called out in the hardening backlog:
// the PnlTracker unit tests cover the P&L math, but nothing exercised
// the OrderProcessor glue that flips trading_enabled on breach. This
// does.

#include "order_gateway/messaging/publishers/api/exec_report_publisher.h"
#include "order_gateway/metrics/metrics.h"
#include "order_gateway/order/inbound_order_events.h"
#include "order_gateway/order/order_processor.h"
#include "order_gateway/order/order_state_manager.h"
#include "order_gateway/risk/pnl_tracker.h"
#include "order_gateway/risk/risk_checker.h"

#include <messages/AccountSnapshot.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>
#include <messages/TimeInForce.h>

#include <cstring>
#include <gtest/gtest.h>
#include <vector>

namespace {

using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectReason;
using bpt::messages::TimeInForce;
using bpt::order_gateway::adapter::ExecEvent;
using bpt::order_gateway::adapter::IOrderAdapter;
using bpt::order_gateway::messaging::api::ExecReportPublisher;
using bpt::order_gateway::metrics::OrderGatewayMetrics;
using bpt::order_gateway::order::NewOrderEvent;
using bpt::order_gateway::order::OrderProcessor;
using bpt::order_gateway::order::OrderStateManager;
using bpt::order_gateway::risk::PnlTracker;
using bpt::order_gateway::risk::PreTradeRiskGate;
using bpt::order_gateway::risk::RejectRateBreaker;
using bpt::order_gateway::risk::RiskChecker;

// Capturing fake: stores every publish() call in-memory so the test
// can assert post-conditions without a real Aeron publication.
struct CapturingExecReportPublisher final : public ExecReportPublisher {
    struct Entry {
        uint64_t order_id;
        ExecStatus::Value status;
        RejectReason::Value reject_reason;
    };

    std::vector<Entry> entries;

    void publish(const bpt::order_gateway::messaging::api::ExecReport& report) override {
        entries.push_back({report.order_id, report.status, report.reject_reason});
    }
};

constexpr int64_t kScale = 100'000'000LL;  // 1e8 fixed-point

ExecEvent make_fill(uint64_t order_id,
                    ExchangeId::Value exchange,
                    uint64_t instrument_id,
                    OrderSide::Value side,
                    int64_t price_e8,
                    uint64_t qty_e8) {
    ExecEvent ev{};
    ev.order_id = order_id;
    ev.exchange_id = exchange;
    ev.instrument_id = instrument_id;
    ev.status = ExecStatus::FILLED;
    ev.side = side;
    ev.order_type = OrderType::LIMIT;
    ev.price = price_e8;
    ev.filled_qty = qty_e8;
    ev.remaining_qty = 0;
    ev.reject_reason = RejectReason::OK;
    ev.fee = 0;
    ev.fee_currency = "USDT";
    // Use a stable "now" so the UTC-day rollover in PnlTracker can't
    // accidentally wipe P&L mid-test. 1e18 ns ≈ 2001-09-09.
    ev.exchange_ts_ns = 1'700'000'000'000'000'000ULL;
    ev.local_ts_ns = 1'700'000'000'000'000'000ULL;
    return ev;
}

// Minimal REJECTED ExecEvent for breaker tests. Uses a unique
// `local_ts_ns` so the rolling window sees distinct events even when
// we feed a tight burst in one test.
ExecEvent make_reject(uint64_t order_id, uint64_t local_ts_ns) {
    ExecEvent ev{};
    ev.order_id = order_id;
    ev.exchange_id = ExchangeId::OKX;
    ev.instrument_id = 100;
    ev.status = ExecStatus::REJECTED;
    ev.side = OrderSide::BUY;
    ev.order_type = OrderType::LIMIT;
    ev.price = 0;
    ev.filled_qty = 0;
    ev.remaining_qty = 0;
    ev.reject_reason = RejectReason::EXCHANGE_ERROR;
    ev.fee = 0;
    ev.fee_currency = "USDT";
    ev.exchange_ts_ns = local_ts_ns;
    ev.local_ts_ns = local_ts_ns;
    return ev;
}

ExecEvent make_ack(uint64_t order_id, uint64_t local_ts_ns) {
    ExecEvent ev{};
    ev.order_id = order_id;
    ev.exchange_id = ExchangeId::OKX;
    ev.instrument_id = 100;
    ev.status = ExecStatus::ACKED;
    ev.side = OrderSide::BUY;
    ev.order_type = OrderType::LIMIT;
    ev.price = 100 * kScale;
    ev.filled_qty = 0;
    ev.remaining_qty = kScale;
    ev.reject_reason = RejectReason::OK;
    ev.fee = 0;
    ev.fee_currency = "USDT";
    ev.exchange_ts_ns = local_ts_ns;
    ev.local_ts_ns = local_ts_ns;
    return ev;
}

NewOrderEvent make_new_order(uint64_t order_id,
                             ExchangeId::Value exchange,
                             uint64_t instrument_id,
                             OrderSide::Value side,
                             int64_t price_e8,
                             uint64_t qty_e8) {
    return NewOrderEvent{
        .order_id = order_id,
        .instrument_id = instrument_id,
        .exchange_id = exchange,
        .side = side,
        .order_type = OrderType::LIMIT,
        .tif = TimeInForce::GTC,
        .price = price_e8,
        .quantity = qty_e8,
        .exchange_symbol = "BTC-USDT",
    };
}

// Build a full OrderProcessor with sensible defaults for tests. Adapters
// vector is empty — the tests here don't route to exchanges, they only
// exercise the exec-event and new-order paths.
struct Harness {
    CapturingExecReportPublisher pub;
    OrderStateManager state_mgr;
    RiskChecker risk_checker{
        /*max_order_size_usd=*/1e9,
        /*max_notional_per_order_usd=*/1e9,
        /*max_open_orders_per_venue=*/1000,
        /*max_orders_per_second=*/1000,
    };
    PnlTracker pnl_tracker;
    PreTradeRiskGate risk_gate;
    OrderGatewayMetrics metrics{0};  // port=0 → no HTTP exposer
    std::vector<std::shared_ptr<IOrderAdapter>> adapters;
    OrderProcessor processor;

    Harness(double max_daily_loss_usd = 10.0, double max_position_usd = 0.0, RejectRateBreaker::Config breaker_cfg = {})
        : risk_gate(risk_checker, pnl_tracker, max_position_usd, max_daily_loss_usd, breaker_cfg),
          processor(pub, state_mgr, risk_gate, metrics, adapters) {}
};

TEST(OrderProcessorRiskLatchTest, TradingEnabledUntilLossCrossesThreshold) {
    Harness h(/*max_daily_loss_usd=*/10.0);
    EXPECT_TRUE(h.risk_checker.trading_enabled());

    // Open long 1 BTC @ $100 on OKX.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    EXPECT_TRUE(h.risk_checker.trading_enabled());

    // Close at $95 → realize -$5. Still above -$10 threshold.
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL, 95 * kScale, 1 * kScale));
    EXPECT_TRUE(h.risk_checker.trading_enabled()) << "−$5 realized should not trip the $10 daily-loss threshold";
}

TEST(OrderProcessorRiskLatchTest, KillSwitchFlipsOnBreach) {
    Harness h(/*max_daily_loss_usd=*/10.0);

    // Build up a -$15 loss in two trades.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL, 85 * kScale, 1 * kScale));

    EXPECT_FALSE(h.risk_checker.trading_enabled())
        << "Realized -$15 > configured $10 cap → kill switch should have flipped";
}

TEST(OrderProcessorRiskLatchTest, LatchStaysSetEvenIfPnlRecovers) {
    Harness h(/*max_daily_loss_usd=*/10.0);

    // Breach: -$15.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL, 85 * kScale, 1 * kScale));
    EXPECT_FALSE(h.risk_checker.trading_enabled());

    // "Recover": fictional profitable trades that take daily P&L back above
    // -$10. The latch is intentionally NOT auto-cleared — operators must
    // restart the service.
    h.processor.on_exec_event(make_fill(3, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(4, ExchangeId::OKX, 100, OrderSide::SELL, 120 * kScale, 1 * kScale));
    // Pretty daily P&L is now -$15 + $20 = +$5, but latch stays.
    EXPECT_FALSE(h.risk_checker.trading_enabled())
        << "Latch must NOT auto-clear on P&L recovery — human review required";
}

TEST(OrderProcessorRiskLatchTest, NewOrderRejectsAfterLatch) {
    Harness h(/*max_daily_loss_usd=*/10.0);

    // Trip the latch.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL, 85 * kScale, 1 * kScale));
    ASSERT_FALSE(h.risk_checker.trading_enabled());

    // Now a NewOrder comes in. Should be rejected via the pretrade
    // RiskChecker.check path with RISK_REJECTED, not routed to an
    // adapter. (Adapters list is empty anyway; if trading were still
    // enabled we'd see an EXCHANGE_ERROR reject for "adapter not
    // connected". RISK_REJECTED proves the risk gate fired first.)
    const size_t before = h.pub.entries.size();
    h.processor.on_new_order(make_new_order(42, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    ASSERT_GT(h.pub.entries.size(), before);
    const auto& last = h.pub.entries.back();
    EXPECT_EQ(last.order_id, 42u);
    EXPECT_EQ(last.status, ExecStatus::REJECTED);
    EXPECT_EQ(last.reject_reason, RejectReason::RISK_REJECTED)
        << "Reject reason should be RISK_REJECTED (the latch), not EXCHANGE_ERROR";
}

// ----- reject-rate breaker integration --------------------------------------

RejectRateBreaker::Config breaker_enabled(double threshold_pct = 20.0, uint32_t min_events = 10) {
    RejectRateBreaker::Config c;
    c.enabled = true;
    c.threshold_pct = threshold_pct;
    c.min_events = min_events;
    c.window_ns = 60ULL * 1'000'000'000ULL;
    return c;
}

TEST(OrderProcessorRejectBreakerTest, TripsAndLatchesRiskChecker) {
    Harness h(/*max_daily_loss_usd=*/0.0,
              /*max_position_usd=*/0.0,
              breaker_enabled(/*threshold_pct=*/20.0, /*min_events=*/10));
    EXPECT_TRUE(h.risk_checker.trading_enabled());

    // 8 ACKs interleaved with 2 REJECTs = 20% exactly → below the strict-
    // greater threshold, so the breaker must NOT trip.
    uint64_t ts = 1'700'000'000'000'000'000ULL;
    for (int i = 0; i < 8; ++i)
        h.processor.on_exec_event(make_ack(100 + i, ts + i));
    for (int i = 0; i < 2; ++i)
        h.processor.on_exec_event(make_reject(200 + i, ts + 10 + i));
    EXPECT_TRUE(h.risk_checker.trading_enabled()) << "20% exactly should sit at threshold, not trip";

    // One more reject pushes us to 3/11 = 27% → trips.
    h.processor.on_exec_event(make_reject(300, ts + 20));
    EXPECT_FALSE(h.risk_checker.trading_enabled()) << "Crossing threshold should flip RiskChecker latch";
}

TEST(OrderProcessorRejectBreakerTest, DisabledBreakerNeverTrips) {
    // breaker.enabled=false (default Config) — feeding nothing but
    // rejects must leave the latch untouched. Also confirms the
    // daily-loss path stays independent.
    Harness h;  // all defaults: daily loss=10, breaker disabled

    uint64_t ts = 1'700'000'000'000'000'000ULL;
    for (int i = 0; i < 50; ++i)
        h.processor.on_exec_event(make_reject(100 + i, ts + i));
    EXPECT_TRUE(h.risk_checker.trading_enabled()) << "Disabled breaker must not halt trading regardless of reject rate";
}

TEST(OrderProcessorRejectBreakerTest, NewOrderRejectsAfterBreakerTrip) {
    Harness h(/*max_daily_loss_usd=*/0.0,
              /*max_position_usd=*/0.0,
              breaker_enabled(/*threshold_pct=*/10.0, /*min_events=*/5));

    uint64_t ts = 1'700'000'000'000'000'000ULL;
    // 5 rejects / 5 total = 100% → well above 10%.
    for (int i = 0; i < 5; ++i)
        h.processor.on_exec_event(make_reject(100 + i, ts + i));
    ASSERT_FALSE(h.risk_checker.trading_enabled());

    const size_t before = h.pub.entries.size();
    h.processor.on_new_order(make_new_order(42, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    ASSERT_GT(h.pub.entries.size(), before);
    const auto& last = h.pub.entries.back();
    EXPECT_EQ(last.order_id, 42u);
    EXPECT_EQ(last.status, ExecStatus::REJECTED);
    EXPECT_EQ(last.reject_reason, RejectReason::RISK_REJECTED)
        << "NewOrder after breaker trip must reject via pretrade risk gate";
}

TEST(OrderProcessorRiskLatchTest, DisabledWhenMaxDailyLossZero) {
    // max_daily_loss_usd=0 disables the check. Heavy losses should NOT
    // flip the kill switch.
    Harness h(/*max_daily_loss_usd=*/0.0);

    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY, 100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL, 1 * kScale, 1 * kScale));
    // -$99 realized.
    EXPECT_TRUE(h.risk_checker.trading_enabled()) << "max_daily_loss_usd=0 should disable the latch entirely";
}

}  // namespace

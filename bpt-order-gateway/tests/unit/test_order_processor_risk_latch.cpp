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

#include "order_gateway/messaging/i_exec_report_publisher.h"
#include "order_gateway/metrics/metrics.h"
#include "order_gateway/order/order_processor.h"
#include "order_gateway/order/order_state_manager.h"
#include "order_gateway/risk/pnl_tracker.h"
#include "order_gateway/risk/risk_checker.h"

#include <gtest/gtest.h>
#include <messages/AccountSnapshot.h>
#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/MessageHeader.h>
#include <messages/NewOrder.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>
#include <messages/TimeInForce.h>

#include <array>
#include <cstring>
#include <vector>

namespace {

using namespace bpt::order_gateway;
using bpt::messages::ExchangeId;
using bpt::messages::ExecStatus;
using bpt::messages::FeeCurrency;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;
using bpt::messages::RejectReason;
using bpt::messages::TimeInForce;

// Capturing fake: stores every publish() call in-memory so the test
// can assert post-conditions without a real Aeron publication.
struct CapturingExecReportPublisher final : public messaging::IExecReportPublisher {
    struct Entry {
        uint64_t order_id;
        ExecStatus::Value status;
        RejectReason::Value reject_reason;
    };

    std::vector<Entry> entries;

    void publish(uint64_t order_id,
                 uint64_t /*exchange_order_id*/,
                 ExchangeId::Value /*exchange_id*/,
                 uint64_t /*instrument_id*/,
                 ExecStatus::Value status,
                 OrderSide::Value /*side*/,
                 OrderType::Value /*order_type*/,
                 int64_t /*price*/,
                 uint64_t /*filled_qty*/,
                 uint64_t /*remaining_qty*/,
                 RejectReason::Value reject_reason,
                 int64_t /*fee*/,
                 FeeCurrency::Value /*fee_currency*/,
                 uint64_t /*exchange_ts_ns*/,
                 uint64_t /*local_ts_ns*/) override {
        entries.push_back({order_id, status, reject_reason});
    }
};

constexpr int64_t kScale = 100'000'000LL;  // 1e8 fixed-point

adapter::ExecEvent make_fill(uint64_t order_id,
                             ExchangeId::Value exchange,
                             uint64_t instrument_id,
                             OrderSide::Value side,
                             int64_t price_e8,
                             uint64_t qty_e8) {
    adapter::ExecEvent ev{};
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
    ev.fee_currency = FeeCurrency::USDT;
    // Use a stable "now" so the UTC-day rollover in PnlTracker can't
    // accidentally wipe P&L mid-test. 1e18 ns ≈ 2001-09-09.
    ev.exchange_ts_ns = 1'700'000'000'000'000'000ULL;
    ev.local_ts_ns = 1'700'000'000'000'000'000ULL;
    return ev;
}

// Build a NewOrder SBE message so we can feed on_new_order a real
// decoded instance.
struct NewOrderBuf {
    std::array<char, 256> buf{};
    bpt::messages::NewOrder decoded;

    NewOrderBuf(uint64_t order_id,
                ExchangeId::Value exchange,
                uint64_t instrument_id,
                OrderSide::Value side,
                int64_t price_e8,
                uint64_t qty_e8) {
        bpt::messages::NewOrder w;
        w.wrapAndApplyHeader(buf.data(), 0, buf.size())
            .orderId(order_id)
            .exchangeId(exchange)
            .instrumentId(instrument_id)
            .side(side)
            .orderType(OrderType::LIMIT)
            .timeInForce(TimeInForce::GTC)
            .price(price_e8)
            .quantity(qty_e8)
            .timestampNs(0)
            .putExchangeSymbol("BTC-USDT");
        decoded.wrapForDecode(buf.data(),
                              bpt::messages::MessageHeader::encodedLength(),
                              bpt::messages::NewOrder::sbeBlockLength(),
                              bpt::messages::NewOrder::sbeSchemaVersion(),
                              buf.size());
    }
};

// Build a full OrderProcessor with sensible defaults for tests. Adapters
// vector is empty — the tests here don't route to exchanges, they only
// exercise the exec-event and new-order paths.
struct Harness {
    CapturingExecReportPublisher pub;
    order::OrderStateManager state_mgr;
    risk::RiskChecker risk_checker{
        /*max_order_size_usd=*/1e9,
        /*max_notional_per_order_usd=*/1e9,
        /*max_open_orders_per_venue=*/1000,
        /*max_orders_per_second=*/1000,
    };
    risk::PnlTracker pnl_tracker;
    metrics::OrderGatewayMetrics metrics{0};  // port=0 → no HTTP exposer
    std::vector<std::shared_ptr<adapter::IOrderAdapter>> adapters;
    order::OrderProcessor processor;

    Harness(double max_daily_loss_usd = 10.0, double max_position_usd = 0.0)
        : processor(pub, state_mgr, risk_checker, pnl_tracker,
                    max_daily_loss_usd, max_position_usd, metrics, adapters) {}
};

TEST(OrderProcessorRiskLatchTest, TradingEnabledUntilLossCrossesThreshold) {
    Harness h(/*max_daily_loss_usd=*/10.0);
    EXPECT_TRUE(h.risk_checker.trading_enabled());

    // Open long 1 BTC @ $100 on OKX.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY,
                                         100 * kScale, 1 * kScale));
    EXPECT_TRUE(h.risk_checker.trading_enabled());

    // Close at $95 → realize -$5. Still above -$10 threshold.
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL,
                                         95 * kScale, 1 * kScale));
    EXPECT_TRUE(h.risk_checker.trading_enabled())
        << "−$5 realized should not trip the $10 daily-loss threshold";
}

TEST(OrderProcessorRiskLatchTest, KillSwitchFlipsOnBreach) {
    Harness h(/*max_daily_loss_usd=*/10.0);

    // Build up a -$15 loss in two trades.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY,
                                         100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL,
                                         85 * kScale, 1 * kScale));

    EXPECT_FALSE(h.risk_checker.trading_enabled())
        << "Realized -$15 > configured $10 cap → kill switch should have flipped";
}

TEST(OrderProcessorRiskLatchTest, LatchStaysSetEvenIfPnlRecovers) {
    Harness h(/*max_daily_loss_usd=*/10.0);

    // Breach: -$15.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY,
                                         100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL,
                                         85 * kScale, 1 * kScale));
    EXPECT_FALSE(h.risk_checker.trading_enabled());

    // "Recover": fictional profitable trades that take daily P&L back above
    // -$10. The latch is intentionally NOT auto-cleared — operators must
    // restart the service.
    h.processor.on_exec_event(make_fill(3, ExchangeId::OKX, 100, OrderSide::BUY,
                                         100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(4, ExchangeId::OKX, 100, OrderSide::SELL,
                                         120 * kScale, 1 * kScale));
    // Pretty daily P&L is now -$15 + $20 = +$5, but latch stays.
    EXPECT_FALSE(h.risk_checker.trading_enabled())
        << "Latch must NOT auto-clear on P&L recovery — human review required";
}

TEST(OrderProcessorRiskLatchTest, NewOrderRejectsAfterLatch) {
    Harness h(/*max_daily_loss_usd=*/10.0);

    // Trip the latch.
    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY,
                                         100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL,
                                         85 * kScale, 1 * kScale));
    ASSERT_FALSE(h.risk_checker.trading_enabled());

    // Now a NewOrder comes in. Should be rejected via the pretrade
    // RiskChecker.check path with RISK_REJECTED, not routed to an
    // adapter. (Adapters list is empty anyway; if trading were still
    // enabled we'd see an EXCHANGE_ERROR reject for "adapter not
    // connected". RISK_REJECTED proves the risk gate fired first.)
    NewOrderBuf order(42, ExchangeId::OKX, 100, OrderSide::BUY,
                     100 * kScale, 1 * kScale);
    const size_t before = h.pub.entries.size();
    h.processor.on_new_order(order.decoded);
    ASSERT_GT(h.pub.entries.size(), before);
    const auto& last = h.pub.entries.back();
    EXPECT_EQ(last.order_id, 42u);
    EXPECT_EQ(last.status, ExecStatus::REJECTED);
    EXPECT_EQ(last.reject_reason, RejectReason::RISK_REJECTED)
        << "Reject reason should be RISK_REJECTED (the latch), not EXCHANGE_ERROR";
}

TEST(OrderProcessorRiskLatchTest, DisabledWhenMaxDailyLossZero) {
    // max_daily_loss_usd=0 disables the check. Heavy losses should NOT
    // flip the kill switch.
    Harness h(/*max_daily_loss_usd=*/0.0);

    h.processor.on_exec_event(make_fill(1, ExchangeId::OKX, 100, OrderSide::BUY,
                                         100 * kScale, 1 * kScale));
    h.processor.on_exec_event(make_fill(2, ExchangeId::OKX, 100, OrderSide::SELL,
                                         1 * kScale, 1 * kScale));
    // -$99 realized.
    EXPECT_TRUE(h.risk_checker.trading_enabled())
        << "max_daily_loss_usd=0 should disable the latch entirely";
}

}  // namespace

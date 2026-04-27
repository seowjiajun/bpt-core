// Unit tests for HyperliquidExecDecoder — the multi-slice fill tracker
// that decides when to emit PARTIAL vs FILLED. This logic is load-bearing:
// emitting FILLED too early on a multi-slice fill causes fenrir's
// inventory to diverge from the exchange.

#include "order_gateway/adapter/hyperliquid/hyperliquid_exec_decoder.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

namespace {

namespace json = boost::json;
using bpt::order_gateway::adapter::ExecEvent;
using bpt::order_gateway::adapter::HyperliquidExecDecoder;
using namespace bpt::messages;

static constexpr double kScale = 1e8;

class HLExecParserFixture : public ::testing::Test {
protected:
    HyperliquidExecDecoder parser;
    std::vector<ExecEvent> events;

    void SetUp() override {
        parser.on_exec_event = [this](const ExecEvent& ev) { events.push_back(ev); };
    }

    // Build a single fill JSON object matching HL's userFills shape.
    static json::object make_fill(uint64_t oid, const char* side, const char* px,
                                  const char* sz, const char* fee = "0.01",
                                  int64_t time_ms = 1700000000000LL) {
        json::object f;
        f["oid"] = static_cast<int64_t>(oid);
        f["side"] = side;
        f["px"] = px;
        f["sz"] = sz;
        f["fee"] = fee;
        f["time"] = time_ms;
        return f;
    }

    void inject_fills(std::initializer_list<json::object> fills, uint64_t recv_ns = 999) {
        json::array arr;
        for (auto& f : fills)
            arr.push_back(f);
        parser.handle_fills(arr, recv_ns);
    }
};

// ---------------------------------------------------------------------------
// Single full fill
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, SingleFullFill) {
    // Register order: 0.001 BTC
    parser.register_order(/*exch_oid=*/42, /*client_order_id=*/100, /*original_qty_e8=*/100000);

    inject_fills({make_fill(42, "B", "72000", "0.001")});

    ASSERT_EQ(events.size(), 1u);
    const auto& ev = events[0];
    EXPECT_EQ(ev.order_id, 100u);
    EXPECT_EQ(ev.exchange_order_id, 42u);
    EXPECT_EQ(ev.exchange_id, ExchangeId::HYPERLIQUID);
    EXPECT_EQ(ev.status, ExecStatus::FILLED);
    EXPECT_EQ(ev.side, OrderSide::BUY);
    EXPECT_EQ(ev.price, static_cast<int64_t>(72000.0 * kScale));
    EXPECT_EQ(ev.filled_qty, static_cast<uint64_t>(0.001 * kScale));
    EXPECT_EQ(ev.remaining_qty, 0u);
    EXPECT_EQ(ev.reject_reason, RejectReason::OK);
}

// ---------------------------------------------------------------------------
// Multi-slice fill — the critical path
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, MultiSliceFillEmitsPartialThenFilled) {
    // Order for 1.0 BTC, filled in two slices of 0.3 + 0.7
    parser.register_order(50, 200, static_cast<uint64_t>(1.0 * kScale));

    // First slice: 0.3
    inject_fills({make_fill(50, "B", "70000", "0.3")});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].status, ExecStatus::PARTIAL);
    EXPECT_EQ(events[0].filled_qty, static_cast<uint64_t>(0.3 * kScale));
    EXPECT_GT(events[0].remaining_qty, 0u);

    // Second slice: 0.7 → fully filled
    inject_fills({make_fill(50, "B", "70000", "0.7")});
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].status, ExecStatus::FILLED);
    EXPECT_EQ(events[1].remaining_qty, 0u);
}

TEST_F(HLExecParserFixture, ThreeSliceFill) {
    parser.register_order(60, 300, static_cast<uint64_t>(0.003 * kScale));

    inject_fills({make_fill(60, "A", "71000", "0.001")});
    EXPECT_EQ(events.back().status, ExecStatus::PARTIAL);

    inject_fills({make_fill(60, "A", "71000", "0.001")});
    EXPECT_EQ(events.back().status, ExecStatus::PARTIAL);

    inject_fills({make_fill(60, "A", "71000", "0.001")});
    EXPECT_EQ(events.back().status, ExecStatus::FILLED);
    EXPECT_EQ(events.size(), 3u);
}

// ---------------------------------------------------------------------------
// Unknown oid is dropped
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, UnknownOidDropped) {
    // No register_order for oid 99
    inject_fills({make_fill(99, "B", "72000", "0.001")});
    EXPECT_TRUE(events.empty());
}

// ---------------------------------------------------------------------------
// Register with oid=0 is ignored
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, ZeroOidIgnored) {
    parser.register_order(0, 100, 100000);
    inject_fills({make_fill(0, "B", "72000", "0.001")});
    EXPECT_TRUE(events.empty());
}

// ---------------------------------------------------------------------------
// Sell side
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, SellSideMapped) {
    parser.register_order(70, 400, 100000);
    inject_fills({make_fill(70, "A", "72000", "0.001")});

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].side, OrderSide::SELL);
}

// ---------------------------------------------------------------------------
// Fee parsing
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, FeeParsed) {
    parser.register_order(80, 500, 100000);
    inject_fills({make_fill(80, "B", "72000", "0.001", "0.05")});

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].fee, static_cast<int64_t>(0.05 * kScale));
    EXPECT_EQ(events[0].fee_currency, FeeCurrency::USDT);
}

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, ExchangeTimestampConvertedToNs) {
    parser.register_order(90, 600, 100000);
    inject_fills({make_fill(90, "B", "72000", "0.001", "0.01", 1700000000000LL)});

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].exchange_ts_ns, 1700000000000ULL * 1'000'000ULL);
    EXPECT_EQ(events[0].local_ts_ns, 999u);
}

// ---------------------------------------------------------------------------
// Cleanup after FILLED — subsequent fills for same oid are dropped
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, CleanupAfterFilled) {
    parser.register_order(42, 100, 100000);
    inject_fills({make_fill(42, "B", "72000", "0.001")});
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].status, ExecStatus::FILLED);

    // Same oid again — should be dropped (pending_ erased after FILLED)
    inject_fills({make_fill(42, "B", "72000", "0.001")});
    EXPECT_EQ(events.size(), 1u);  // no new event
}

// ---------------------------------------------------------------------------
// Multiple orders in flight simultaneously
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, MultipleOrdersIndependent) {
    parser.register_order(10, 1000, static_cast<uint64_t>(1.0 * kScale));
    parser.register_order(20, 2000, static_cast<uint64_t>(2.0 * kScale));

    inject_fills({make_fill(10, "B", "70000", "1.0")});
    EXPECT_EQ(events.back().order_id, 1000u);
    EXPECT_EQ(events.back().status, ExecStatus::FILLED);

    inject_fills({make_fill(20, "A", "71000", "1.0")});
    EXPECT_EQ(events.back().order_id, 2000u);
    EXPECT_EQ(events.back().status, ExecStatus::PARTIAL);

    inject_fills({make_fill(20, "A", "71000", "1.0")});
    EXPECT_EQ(events.back().order_id, 2000u);
    EXPECT_EQ(events.back().status, ExecStatus::FILLED);
}

// ---------------------------------------------------------------------------
// Batch of fills in a single handle_fills call
// ---------------------------------------------------------------------------

TEST_F(HLExecParserFixture, BatchFills) {
    parser.register_order(10, 1000, static_cast<uint64_t>(0.5 * kScale));
    parser.register_order(20, 2000, static_cast<uint64_t>(0.5 * kScale));

    inject_fills({
        make_fill(10, "B", "70000", "0.5"),
        make_fill(20, "A", "71000", "0.5"),
    });

    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].order_id, 1000u);
    EXPECT_EQ(events[0].status, ExecStatus::FILLED);
    EXPECT_EQ(events[1].order_id, 2000u);
    EXPECT_EQ(events[1].status, ExecStatus::FILLED);
}

}  // namespace

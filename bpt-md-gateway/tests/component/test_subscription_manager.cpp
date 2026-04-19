// Component tests for SubscriptionManager.
//
// Uses a mock IAdapter (GMock) and FakeAckPublisher to verify subscribe/unsubscribe
// routing and ack publishing without any network or Aeron.

#include "fake_ack_publisher.h"
#include "md_gateway/adapter/common/i_adapter.h"
#include "md_gateway/subscription/subscription_manager.h"

#include <messages/AckStatus.h>
#include <messages/MdSubscribeBatch.h>
#include <messages/MessageHeader.h>
#include <messages/TradeSide.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace bpt::md_gateway::subscription {
namespace {

using namespace bpt::messages;
using ::testing::_;

// ── Mock adapter ──────────────────────────────────────────────────────────────

class MockAdapter : public adapter::IAdapter {
public:
    explicit MockAdapter(std::string name) : name_(std::move(name)) {}
    [[nodiscard]] const char* exchange_name() const override { return name_.c_str(); }
    [[nodiscard]] bpt::common::util::LatencyHistogram& decode_latency_hist() noexcept override { return hist_; }

    MOCK_METHOD(void, subscribe, (uint64_t, std::string, uint8_t), (override));
    MOCK_METHOD(void, unsubscribe, (uint64_t), (override));
    MOCK_METHOD(void, start, (), (override));
    MOCK_METHOD(void, stop, (), (override));

    [[nodiscard]] uint64_t md_published_count() const noexcept override { return 0; }
    [[nodiscard]] uint64_t validation_drop_count() const noexcept override { return 0; }

private:
    std::string name_;
    bpt::common::util::LatencyHistogram hist_;
};

// ── Helpers ───────────────────────────────────────────────────────────────────

// Build a one-instrument MdSubscribeBatch into a flat buffer and return the decoded message.
// Uses SBE encoding in a scratch buffer.
struct BatchBuffer {
    static constexpr std::size_t kBufSize = 512;
    char buf[kBufSize]{};
    MdSubscribeBatch msg;

    BatchBuffer(uint64_t correlation_id,
                const std::vector<std::tuple<uint64_t, std::string, std::string, uint8_t>>& instruments) {
        MessageHeader hdr(buf, kBufSize);
        hdr.blockLength(MdSubscribeBatch::sbeBlockLength())
            .templateId(MdSubscribeBatch::sbeTemplateId())
            .schemaId(MdSubscribeBatch::sbeSchemaId())
            .version(MdSubscribeBatch::sbeSchemaVersion());

        msg.wrapForEncode(buf, MessageHeader::encodedLength(), kBufSize);
        msg.correlationId(correlation_id);

        auto& g = msg.instrumentsCount(static_cast<uint32_t>(instruments.size()));
        for (const auto& [id, exch, sym, depth] : instruments) {
            g.next().instrumentId(id).depth(depth).putExchange(exch.c_str()).putSymbol(sym.c_str());
        }
    }

    MdSubscribeBatch& decode() {
        MessageHeader hdr(buf, kBufSize);
        msg.wrapForDecode(buf, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), kBufSize);
        return msg;
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST(SubscriptionManagerTest, ApplyBatchSubscribesNewInstrument) {
    auto mock = std::make_shared<MockAdapter>("BINANCE");
    EXPECT_CALL(*mock, subscribe(1001, std::string("BTCUSDT"), 0)).Times(1);

    test::FakeAckPublisher ack;
    SubscriptionManager mgr;
    mgr.add_adapter(mock);

    BatchBuffer batch(42, {{1001, "BINANCE", "BTCUSDT", 0}});
    mgr.apply_batch(batch.decode(), ack);

    EXPECT_EQ(mgr.active_count(), 1u);
    ASSERT_EQ(ack.acks.size(), 1u);
    EXPECT_EQ(ack.acks[0].correlation_id, 42u);
    EXPECT_EQ(ack.acks[0].instrument_id, 1001u);
    EXPECT_EQ(ack.acks[0].exchange, "BINANCE");
    EXPECT_EQ(ack.acks[0].status, AckStatus::OK);
}

TEST(SubscriptionManagerTest, ApplyBatchUnsubscribesRemovedInstrument) {
    auto mock = std::make_shared<MockAdapter>("BINANCE");
    EXPECT_CALL(*mock, subscribe(1001, _, _)).Times(1);
    EXPECT_CALL(*mock, unsubscribe(1001)).Times(1);

    test::FakeAckPublisher ack;
    SubscriptionManager mgr;
    mgr.add_adapter(mock);

    // Subscribe
    {
        BatchBuffer batch(1, {{1001, "BINANCE", "BTCUSDT", 0}});
        mgr.apply_batch(batch.decode(), ack);
    }
    EXPECT_EQ(mgr.active_count(), 1u);

    // Replace with empty batch → unsubscribe
    ack.reset();
    {
        BatchBuffer batch(2, {});
        mgr.apply_batch(batch.decode(), ack);
    }
    EXPECT_EQ(mgr.active_count(), 0u);
    EXPECT_EQ(ack.acks.size(), 0u);  // No acks for empty batch
}

TEST(SubscriptionManagerTest, DuplicateSubscribeNotReissued) {
    auto mock = std::make_shared<MockAdapter>("BINANCE");
    // subscribe must only be called once even if batch is applied twice
    EXPECT_CALL(*mock, subscribe(1001, _, _)).Times(1);

    test::FakeAckPublisher ack;
    SubscriptionManager mgr;
    mgr.add_adapter(mock);

    BatchBuffer batch1(1, {{1001, "BINANCE", "BTCUSDT", 0}});
    mgr.apply_batch(batch1.decode(), ack);

    ack.reset();
    BatchBuffer batch2(2, {{1001, "BINANCE", "BTCUSDT", 0}});
    mgr.apply_batch(batch2.decode(), ack);

    EXPECT_EQ(mgr.active_count(), 1u);
    // Ack still sent on second apply
    ASSERT_EQ(ack.acks.size(), 1u);
    EXPECT_EQ(ack.acks[0].status, AckStatus::OK);
}

TEST(SubscriptionManagerTest, UnknownExchangeAcksNotFound) {
    auto mock = std::make_shared<MockAdapter>("BINANCE");
    EXPECT_CALL(*mock, subscribe(_, _, _)).Times(0);

    test::FakeAckPublisher ack;
    SubscriptionManager mgr;
    mgr.add_adapter(mock);

    BatchBuffer batch(99, {{2001, "OKX", "BTC-USDT-SWAP", 0}});
    mgr.apply_batch(batch.decode(), ack);

    EXPECT_EQ(mgr.active_count(), 0u);
    ASSERT_EQ(ack.acks.size(), 1u);
    EXPECT_EQ(ack.acks[0].status, AckStatus::NOT_FOUND);
}

TEST(SubscriptionManagerTest, PublishSubscriptionHeartbeats) {
    auto mock = std::make_shared<MockAdapter>("BINANCE");
    EXPECT_CALL(*mock, subscribe(_, _, _)).Times(2);

    test::FakeAckPublisher ack;
    SubscriptionManager mgr;
    mgr.add_adapter(mock);

    BatchBuffer batch(1, {{1001, "BINANCE", "BTCUSDT", 0}, {1002, "BINANCE", "ETHUSDT", 0}});
    mgr.apply_batch(batch.decode(), ack);
    EXPECT_EQ(mgr.active_count(), 2u);

    ack.reset();
    mgr.publish_subscription_heartbeats(ack);
    EXPECT_EQ(ack.subscription_heartbeats.size(), 2u);
}

}  // namespace
}  // namespace bpt::md_gateway::subscription

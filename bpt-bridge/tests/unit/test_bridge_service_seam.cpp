/// \file
/// \brief Drives BridgeService's IBroadcaster + api::DashboardControlPublisher seam
/// through fake port implementations. No Aeron driver, no real WebSocket
/// listener — bus_ is default-constructed (all unique_ptrs null) and the
/// poll loop never runs. We drive event handlers directly.
///
/// Locks in:
///   - decode-and-forward path for each input stream (exec → broadcast Order;
///     toxicity → broadcast Toxicity; portfolio JSON → broadcast Order)
///   - dashboard HALT/RESUME command protocol (string → ctrl_sink byte +
///     status broadcast)

#include "bridge/app/bridge_service.h"
#include "bridge/messaging/aeron_bus.h"
#include "bridge/messaging/publishers/api/dashboard_control_publisher.h"
#include "bridge/messaging/subscribers/api/exec_subscriber.h"
#include "bridge/ws/i_broadcaster.h"
#include "bridge/ws/message_encoder.h"
#include "bridge/ws/msg_kind.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {

using bpt::bridge::BridgeService;
using bpt::bridge::MsgKind;
using bpt::bridge::messaging::api::ExecSubscriber;
namespace ctrl_api = bpt::bridge::messaging::api;
using bpt::bridge::ws::IBroadcaster;

// ── Fakes ────────────────────────────────────────────────────────────────────

class FakeBroadcaster final : public IBroadcaster {
public:
    struct Entry {
        MsgKind kind;
        std::string payload;
    };
    std::vector<Entry> messages;
    CommandHandler installed_handler;

    void publish(MsgKind kind, std::string message) override {
        messages.push_back({kind, std::move(message)});
    }

    void set_command_handler(CommandHandler h) override { installed_handler = std::move(h); }

    /// Convenience: count messages by kind.
    std::size_t count(MsgKind k) const {
        std::size_t n = 0;
        for (const auto& m : messages)
            if (m.kind == k)
                ++n;
        return n;
    }

    /// Convenience: last payload of a given kind, or empty if none.
    std::string last(MsgKind k) const {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            if (it->kind == k)
                return it->payload;
        return {};
    }
};

class FakeCtrlSink final : public ctrl_api::DashboardControlPublisher {
public:
    int halts = 0;
    int resumes = 0;

    void publish_halt() override { ++halts; }
    void publish_resume() override { ++resumes; }
};

// ── Helpers ──────────────────────────────────────────────────────────────────

bpt::bridge::config::Settings make_test_settings() {
    bpt::bridge::config::Settings s;
    s.symbol = "BTC-USDT";
    s.strategy = "TestStrategy";
    s.exchange = "OKX";
    s.mode = "paper";
    s.instrument_type = "PERP";
    s.instrument_id = 0;  // accept all instruments
    return s;
}

std::unique_ptr<BridgeService> make_service(std::shared_ptr<FakeBroadcaster> bc,
                                            std::shared_ptr<FakeCtrlSink> ctrl) {
    return std::make_unique<BridgeService>(make_test_settings(),
                                           bpt::bridge::messaging::BridgeBus{},
                                           std::move(bc),
                                           std::move(ctrl));
}

}  // namespace

// ── Tests ────────────────────────────────────────────────────────────────────

TEST(BridgeServiceSeamTest, ExecOrderEventBroadcastsOrderMessage) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();
    auto svc = make_service(bc, ctrl);

    ExecSubscriber::OrderEvent ev{};
    ev.ts_ns = 1'700'000'000'000'000'000ULL;
    ev.order_id = 42;
    ev.instrument_id = 100;
    ev.side = bpt::bridge::encode::Side::Buy;
    ev.status = 0;  // ACKED
    ev.order_type = 1;  // LIMIT
    ev.price = 30000.5;
    ev.qty = 1.0;
    ev.filled_qty = 0.0;
    ev.remaining_qty = 1.0;

    svc->on_exec_order_event(ev);

    EXPECT_EQ(bc->count(MsgKind::Order), 1u);
    const auto payload = bc->last(MsgKind::Order);
    EXPECT_NE(payload.find("\"orderId\":42"), std::string::npos) << payload;
    EXPECT_NE(payload.find("\"status\":\"acked\""), std::string::npos) << payload;
}

TEST(BridgeServiceSeamTest, ExecFillBroadcastsFillAndPositionMessages) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();
    auto svc = make_service(bc, ctrl);

    ExecSubscriber::Fill f{};
    f.ts_ns = 1'700'000'000'000'000'000ULL;
    f.order_id = 7;
    f.instrument_id = 100;
    f.side = bpt::bridge::encode::Side::Buy;
    f.order_type = 1;
    f.qty = 0.5;
    f.price = 30000.0;
    f.fee = 0.01;

    svc->on_exec_fill(f);

    EXPECT_EQ(bc->count(MsgKind::Fill), 1u);
    EXPECT_EQ(bc->count(MsgKind::Position), 1u);
    EXPECT_NE(bc->last(MsgKind::Fill).find("\"orderId\":7"), std::string::npos);
}

TEST(BridgeServiceSeamTest, InstrumentFilterDropsFillsForOtherInstruments) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();

    auto settings = make_test_settings();
    settings.instrument_id = 100;  // only forward instrument 100
    auto svc = std::make_unique<BridgeService>(std::move(settings),
                                               bpt::bridge::messaging::BridgeBus{},
                                               bc,
                                               ctrl);

    ExecSubscriber::Fill f{};
    f.instrument_id = 999;  // not the filtered one
    f.qty = 1.0;
    f.price = 100.0;
    svc->on_exec_fill(f);

    EXPECT_EQ(bc->count(MsgKind::Fill), 0u);
    EXPECT_EQ(bc->count(MsgKind::Position), 0u);
}

TEST(BridgeServiceSeamTest, DashboardHaltCommandPublishesByteAndStatus) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();
    auto svc = make_service(bc, ctrl);

    svc->on_dashboard_command("halt");

    EXPECT_EQ(ctrl->halts, 1);
    EXPECT_EQ(ctrl->resumes, 0);
    EXPECT_EQ(bc->count(MsgKind::Status), 1u);
    EXPECT_NE(bc->last(MsgKind::Status).find("\"state\":\"halted\""), std::string::npos);
}

TEST(BridgeServiceSeamTest, DashboardResumeCommandPublishesByteAndStatus) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();
    auto svc = make_service(bc, ctrl);

    svc->on_dashboard_command("resume");

    EXPECT_EQ(ctrl->halts, 0);
    EXPECT_EQ(ctrl->resumes, 1);
    EXPECT_NE(bc->last(MsgKind::Status).find("\"state\":\"live\""), std::string::npos);
}

TEST(BridgeServiceSeamTest, UnknownCommandIsIgnored) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();
    auto svc = make_service(bc, ctrl);

    svc->on_dashboard_command("flip");

    EXPECT_EQ(ctrl->halts, 0);
    EXPECT_EQ(ctrl->resumes, 0);
    EXPECT_EQ(bc->messages.size(), 0u);
}

TEST(BridgeServiceSeamTest, PortfolioJsonForwardedAsOrderMessage) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();
    auto svc = make_service(bc, ctrl);

    const std::string json = R"({"strategy":"OptionsMaker","equity":100000})";
    svc->on_portfolio_json(json);

    EXPECT_EQ(bc->count(MsgKind::Order), 1u);
    EXPECT_EQ(bc->last(MsgKind::Order), json);
}

TEST(BridgeServiceSeamTest, ToxicityUpdateFormatsCorrectJson) {
    auto bc = std::make_shared<FakeBroadcaster>();
    auto ctrl = std::make_shared<FakeCtrlSink>();
    auto svc = make_service(bc, ctrl);

    bpt::analytics::messaging::ToxicityUpdate u{};
    u.bid_markout_5s_bps = 1.5;
    u.ask_markout_5s_bps = -2.0;
    u.bid_sample_count = 100;
    u.ask_sample_count = 95;
    u.bid_toxicity_score = 0.42;
    u.ask_toxicity_score = 0.51;
    u.bid_fill_rate = 0.85;
    u.ask_fill_rate = 0.78;
    u.bid_ttf_ms = 12.5;
    u.ask_ttf_ms = 13.7;

    svc->on_toxicity(u);

    EXPECT_EQ(bc->count(MsgKind::Toxicity), 1u);
    const auto payload = bc->last(MsgKind::Toxicity);
    // Spot-check a few fields are present in the JSON.
    EXPECT_NE(payload.find("bidSamples"), std::string::npos) << payload;
    EXPECT_NE(payload.find("askSamples"), std::string::npos) << payload;
}

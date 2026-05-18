/// \file
/// \brief Drives RefdataService's bus seam through fake port implementations.
///
/// Locks in the hexagonal flip: RefdataService depends only on the IRefdata*Sink
/// / IRefdataControlSource interfaces, not on Aeron. If a future change leaks
/// a concrete Aeron type back into RefdataService's public surface, this test stops
/// compiling.

#include "refdata/app/refdata_service.h"
#include "refdata/config/settings.h"
#include "refdata/messaging/messages.h"
#include "refdata/messaging/publishers/api/fee_schedule_publisher.h"
#include "refdata/messaging/subscribers/api/refdata_control_subscriber.h"
#include "refdata/messaging/publishers/api/refdata_delta_publisher.h"
#include "refdata/messaging/publishers/api/refdata_snapshot_publisher.h"
#include "refdata/messaging/publishers/api/refdata_status_publisher.h"
#include "refdata/model/funding_rate.h"
#include "refdata/model/instrument.h"
#include "refdata/registry/instrument_registry.h"

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace {

using bpt::refdata::RefdataService;
using bpt::refdata::config::Settings;
using bpt::refdata::messaging::RefdataRequest;
using bpt::refdata::messaging::api::FeeSchedulePublisher;
using bpt::refdata::messaging::api::RefdataControlSubscriber;
using bpt::refdata::messaging::api::RefdataDeltaPublisher;
using bpt::refdata::messaging::api::RefdataSnapshotPublisher;
using bpt::refdata::messaging::api::RefdataStatusPublisher;
using bpt::refdata::model::FeeScheduleState;
using bpt::refdata::model::Instrument;
using bpt::refdata::registry::InstrumentRegistry;

// ---- Fakes -----------------------------------------------------------------

class FakeRefdataControlSource final : public RefdataControlSubscriber {
public:
    int poll(RequestHandler /*handler*/) override { return 0; }
};

class FakeRefdataSnapshotSink final : public RefdataSnapshotPublisher {
public:
    struct Call {
        uint64_t correlation_id;
        uint64_t seq_start;
        std::size_t instrument_count;
    };
    std::vector<Call> calls;

    void publish(const InstrumentRegistry& reg,
                 const RefdataRequest& request,
                 uint64_t seq_start) override {
        calls.push_back({request.correlation_id, seq_start, reg.count()});
    }
};

class FakeRefdataDeltaSink final : public RefdataDeltaPublisher {
public:
    uint64_t seq{42};
    std::size_t deltas{0};
    std::size_t heartbeats{0};

    void publish_delta(bpt::messages::DeltaUpdateType::Value /*ut*/, const Instrument& /*inst*/) override {
        ++deltas;
    }
    void publish_heartbeat() override { ++heartbeats; }
    uint64_t current_sequence() const override { return seq; }
};

class FakeFeeScheduleSink final : public FeeSchedulePublisher {
public:
    std::size_t calls{0};
    void publish(const FeeScheduleState& /*fs*/) override { ++calls; }
};

class FakeRefdataStatusSink final : public RefdataStatusPublisher {
public:
    std::size_t ready_calls{0};
    std::size_t error_calls{0};
    void publish_ready(uint8_t /*exchanges*/, uint16_t /*instruments*/, bool /*fees*/) override { ++ready_calls; }
    void publish_error(bpt::messages::RefDataErrorType::Value /*err*/,
                       bpt::messages::ExchangeId::Value /*exch*/,
                       uint64_t /*inst_id*/) override {
        ++error_calls;
    }
};

Settings make_test_settings() {
    Settings s;
    s.base.metrics_port = 0;               // skip Prometheus exposer
    s.instrument_mapping.local_path = "";  // skip mapping JSON load
    return s;
}

}  // namespace

TEST(RefdataServiceSeamTest, HandleRequestForwardsToSnapshotSink) {
    auto control = std::make_unique<FakeRefdataControlSource>();
    auto snapshot = std::make_unique<FakeRefdataSnapshotSink>();
    auto delta = std::make_shared<FakeRefdataDeltaSink>();
    auto fee = std::make_shared<FakeFeeScheduleSink>();
    auto status = std::make_shared<FakeRefdataStatusSink>();

    auto* snapshot_obs = snapshot.get();
    const uint64_t expected_seq = delta->seq;

    RefdataService app(make_test_settings(), std::move(control), std::move(snapshot), delta, fee, status, {});

    RefdataRequest req{};
    req.correlation_id = 7777;

    app.handle_request(req);

    ASSERT_EQ(snapshot_obs->calls.size(), std::size_t{1});
    EXPECT_EQ(snapshot_obs->calls[0].correlation_id, uint64_t{7777});
    // Seq stamp on the snapshot must come from the delta sink's current
    // sequence — that's how subscribers join the delta stream gap-free.
    EXPECT_EQ(snapshot_obs->calls[0].seq_start, expected_seq);
}

TEST(RefdataServiceSeamTest, ConstructsWithoutAeron) {
    // If anything in RefdataService's public surface starts pulling Aeron
    // headers transitively, this TU stops compiling — we never include
    // <Aeron.h> here.
    auto control = std::make_unique<FakeRefdataControlSource>();
    auto snapshot = std::make_unique<FakeRefdataSnapshotSink>();
    auto delta = std::make_shared<FakeRefdataDeltaSink>();
    auto fee = std::make_shared<FakeFeeScheduleSink>();
    auto status = std::make_shared<FakeRefdataStatusSink>();

    EXPECT_NO_THROW(
        { RefdataService app(make_test_settings(), std::move(control), std::move(snapshot), delta, fee, status, {}); });
}

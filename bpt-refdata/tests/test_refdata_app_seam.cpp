/// \file
/// \brief Drives RefdataApp's bus seam through fake port implementations.
///
/// Locks in the hexagonal flip: RefdataApp depends only on the IRefdata*Sink
/// / IRefdataControlSource interfaces, not on Aeron. If a future change leaks
/// a concrete Aeron type back into RefdataApp's public surface, this test stops
/// compiling.

#include "refdata/app/refdata_app.h"
#include "refdata/config/settings.h"
#include "refdata/messaging/messages.h"
#include "refdata/port/i_fee_schedule_sink.h"
#include "refdata/port/i_refdata_control_source.h"
#include "refdata/port/i_refdata_delta_sink.h"
#include "refdata/port/i_refdata_snapshot_sink.h"
#include "refdata/port/i_refdata_status_sink.h"
#include "refdata/refdata/funding_rate.h"
#include "refdata/refdata/instrument.h"
#include "refdata/registry/instrument_registry.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

using namespace bpt::refdata;

// ---- Fakes -----------------------------------------------------------------

class FakeRefdataControlSource final : public port::IRefdataControlSource {
public:
    int poll(RequestHandler /*handler*/) override { return 0; }
};

class FakeRefdataSnapshotSink final : public port::IRefdataSnapshotSink {
public:
    struct Call {
        uint64_t correlation_id;
        uint64_t seq_start;
        std::size_t instrument_count;
    };
    std::vector<Call> calls;

    void publish(const registry::InstrumentRegistry& reg,
                 const messaging::RefdataRequest& request,
                 uint64_t seq_start) override {
        calls.push_back({request.correlation_id, seq_start, reg.count()});
    }
};

class FakeRefdataDeltaSink final : public port::IRefdataDeltaSink {
public:
    uint64_t seq{42};
    std::size_t deltas{0};
    std::size_t heartbeats{0};

    void publish_delta(bpt::messages::DeltaUpdateType::Value /*ut*/,
                       const refdata::Instrument& /*inst*/) override {
        ++deltas;
    }
    void publish_heartbeat() override { ++heartbeats; }
    uint64_t current_sequence() const override { return seq; }
};

class FakeFeeScheduleSink final : public port::IFeeScheduleSink {
public:
    std::size_t calls{0};
    void publish(const refdata::FeeScheduleState& /*fs*/) override { ++calls; }
};

class FakeRefdataStatusSink final : public port::IRefdataStatusSink {
public:
    std::size_t ready_calls{0};
    std::size_t error_calls{0};
    void publish_ready(uint8_t /*exchanges*/,
                       uint16_t /*instruments*/,
                       bool /*fees*/) override {
        ++ready_calls;
    }
    void publish_error(bpt::messages::RefDataErrorType::Value /*err*/,
                       bpt::messages::ExchangeId::Value /*exch*/,
                       uint64_t /*inst_id*/) override {
        ++error_calls;
    }
};

config::Settings make_test_settings() {
    config::Settings s;
    s.base.metrics_port = 0;               // skip Prometheus exposer
    s.instrument_mapping.local_path = "";  // skip mapping JSON load
    return s;
}

}  // namespace

TEST(RefdataAppSeamTest, HandleRequestForwardsToSnapshotSink) {
    auto control = std::make_unique<FakeRefdataControlSource>();
    auto snapshot = std::make_unique<FakeRefdataSnapshotSink>();
    auto delta = std::make_shared<FakeRefdataDeltaSink>();
    auto fee = std::make_shared<FakeFeeScheduleSink>();
    auto status = std::make_shared<FakeRefdataStatusSink>();

    auto* snapshot_obs = snapshot.get();
    const uint64_t expected_seq = delta->seq;

    RefdataApp app(make_test_settings(),
                   std::move(control),
                   std::move(snapshot),
                   delta,
                   fee,
                   status,
                   {});

    messaging::RefdataRequest req{};
    req.correlation_id = 7777;

    app.handle_request(req);

    ASSERT_EQ(snapshot_obs->calls.size(), std::size_t{1});
    EXPECT_EQ(snapshot_obs->calls[0].correlation_id, uint64_t{7777});
    // Seq stamp on the snapshot must come from the delta sink's current
    // sequence — that's how subscribers join the delta stream gap-free.
    EXPECT_EQ(snapshot_obs->calls[0].seq_start, expected_seq);
}

TEST(RefdataAppSeamTest, ConstructsWithoutAeron) {
    // If anything in RefdataApp's public surface starts pulling Aeron
    // headers transitively, this TU stops compiling — we never include
    // <Aeron.h> here.
    auto control = std::make_unique<FakeRefdataControlSource>();
    auto snapshot = std::make_unique<FakeRefdataSnapshotSink>();
    auto delta = std::make_shared<FakeRefdataDeltaSink>();
    auto fee = std::make_shared<FakeFeeScheduleSink>();
    auto status = std::make_shared<FakeRefdataStatusSink>();

    EXPECT_NO_THROW({
        RefdataApp app(make_test_settings(),
                       std::move(control),
                       std::move(snapshot),
                       delta,
                       fee,
                       status,
                       {});
    });
}

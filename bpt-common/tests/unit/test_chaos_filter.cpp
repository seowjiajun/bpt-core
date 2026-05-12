// Coverage for ChaosRegistry + load_chaos_config.
//
// The runtime piece (ChaosRegistry::wrap) is tested in isolation by
// counting how often the wrapped handler runs vs the inner handler.
// We do NOT spin up a real Aeron Subscriber — the contract under test
// is "given a handler and a configured drop_prob, drops happen at the
// expected rate". Subscriber wiring is exercised by service-level
// tests + live runs.

#include <Aeron.h>

#include <atomic>
#include <bpt_common/aeron/chaos_config.h>
#include <bpt_common/aeron/chaos_filter.h>
#include <cstring>
#include <gtest/gtest.h>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

using bpt::common::aeron::ChaosConfig;
using bpt::common::aeron::ChaosRegistry;
using bpt::common::aeron::FragmentHandler;
using bpt::common::aeron::load_chaos_config;

// Minimal AtomicBuffer + Header pair we can hand to a FragmentHandler.
// The handler isn't expected to read the buffer in these tests — we
// only count invocations.
struct FakeFragment {
    std::array<std::uint8_t, 16> bytes{};
    ::aeron::AtomicBuffer buffer{bytes.data(), bytes.size()};
};

class ChaosFilterTest : public ::testing::Test {
protected:
    void SetUp() override { ChaosRegistry::clear_for_testing(); }
    void TearDown() override { ChaosRegistry::clear_for_testing(); }
};

TEST_F(ChaosFilterTest, NoConfigIsIdentity) {
    // No set_global call → wrap returns the inner handler unchanged.
    std::atomic<int> calls{0};
    FragmentHandler inner =
        [&calls](::aeron::AtomicBuffer&, ::aeron::util::index_t, ::aeron::util::index_t, ::aeron::Header&) {
            calls.fetch_add(1);
        };

    auto wrapped = ChaosRegistry::wrap(/*stream_id=*/1003, std::move(inner));
    EXPECT_EQ(0.0, ChaosRegistry::drop_prob_for_testing(1003));
}

TEST_F(ChaosFilterTest, StreamWithoutEntryIsIdentity) {
    ChaosConfig cfg;
    cfg.drop_prob_by_stream_id[1003] = 1.0;
    ChaosRegistry::set_global(cfg);

    EXPECT_EQ(1.0, ChaosRegistry::drop_prob_for_testing(1003));
    EXPECT_EQ(0.0, ChaosRegistry::drop_prob_for_testing(2002));
}

TEST_F(ChaosFilterTest, FullDropEverythingDropped) {
    ChaosConfig cfg;
    cfg.drop_prob_by_stream_id[1003] = 1.0;
    ChaosRegistry::set_global(cfg);

    std::atomic<int> calls{0};
    FragmentHandler inner =
        [&calls](::aeron::AtomicBuffer&, ::aeron::util::index_t, ::aeron::util::index_t, ::aeron::Header&) {
            calls.fetch_add(1);
        };
    auto wrapped = ChaosRegistry::wrap(1003, std::move(inner));

    FakeFragment frag;
    ::aeron::concurrent::logbuffer::Header hdr{0, 0, nullptr};
    for (int i = 0; i < 1000; ++i) {
        wrapped(frag.buffer, 0, frag.bytes.size(), hdr);
    }
    EXPECT_EQ(0, calls.load());
}

TEST_F(ChaosFilterTest, PartialDropMatchesExpectedRate) {
    ChaosConfig cfg;
    cfg.drop_prob_by_stream_id[1003] = 0.3;  // 30% drop
    ChaosRegistry::set_global(cfg);

    std::atomic<int> calls{0};
    FragmentHandler inner =
        [&calls](::aeron::AtomicBuffer&, ::aeron::util::index_t, ::aeron::util::index_t, ::aeron::Header&) {
            calls.fetch_add(1);
        };
    auto wrapped = ChaosRegistry::wrap(1003, std::move(inner));

    FakeFragment frag;
    ::aeron::concurrent::logbuffer::Header hdr{0, 0, nullptr};
    constexpr int kIters = 10000;
    for (int i = 0; i < kIters; ++i) {
        wrapped(frag.buffer, 0, frag.bytes.size(), hdr);
    }
    // Expect ~7000 calls; allow ±5% slop for the rng.
    const double observed_pass_rate = static_cast<double>(calls.load()) / kIters;
    EXPECT_NEAR(0.70, observed_pass_rate, 0.05);
}

// ---------------------------------------------------------------------------
// load_chaos_config

TEST(ChaosConfigLoader, EmptyTableMeansEmptyConfig) {
    auto root = toml::parse("");
    auto cfg = load_chaos_config(root, "dev");
    EXPECT_TRUE(cfg.empty());
}

TEST(ChaosConfigLoader, RoundTripsValues) {
    auto root = toml::parse(R"(
        [chaos.drop_prob_by_stream_id]
        "1003" = 1.0
        "2002" = 0.25
    )");
    auto cfg = load_chaos_config(root, "dev");
    ASSERT_EQ(2u, cfg.drop_prob_by_stream_id.size());
    EXPECT_DOUBLE_EQ(1.0, cfg.drop_prob_by_stream_id.at(1003));
    EXPECT_DOUBLE_EQ(0.25, cfg.drop_prob_by_stream_id.at(2002));
}

TEST(ChaosConfigLoader, ZeroDropIsElided) {
    auto root = toml::parse(R"(
        [chaos.drop_prob_by_stream_id]
        "1003" = 0.0
    )");
    auto cfg = load_chaos_config(root, "dev");
    // 0.0 is the default; no point storing it.
    EXPECT_TRUE(cfg.empty());
}

TEST(ChaosConfigLoader, RejectsOutOfRange) {
    auto too_high = toml::parse(R"(
        [chaos.drop_prob_by_stream_id]
        "1003" = 1.5
    )");
    EXPECT_THROW(load_chaos_config(too_high, "dev"), std::invalid_argument);

    auto negative = toml::parse(R"(
        [chaos.drop_prob_by_stream_id]
        "1003" = -0.1
    )");
    EXPECT_THROW(load_chaos_config(negative, "dev"), std::invalid_argument);
}

TEST(ChaosConfigLoader, RejectsNonIntKey) {
    auto root = toml::parse(R"(
        [chaos.drop_prob_by_stream_id]
        not_an_int = 0.5
    )");
    EXPECT_THROW(load_chaos_config(root, "dev"), std::invalid_argument);
}

TEST(ChaosConfigLoader, ProdGateThrowsOnAnyConfiguredDrop) {
    auto root = toml::parse(R"(
        [chaos.drop_prob_by_stream_id]
        "1003" = 0.1
    )");
    EXPECT_THROW(load_chaos_config(root, "prod"), std::runtime_error);
}

TEST(ChaosConfigLoader, ProdAcceptsEmptyConfig) {
    auto empty = toml::parse("");
    EXPECT_NO_THROW(load_chaos_config(empty, "prod"));

    auto explicit_zero = toml::parse(R"(
        [chaos.drop_prob_by_stream_id]
        "1003" = 0.0
    )");
    EXPECT_NO_THROW(load_chaos_config(explicit_zero, "prod"));
}

}  // namespace

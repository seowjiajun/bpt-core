#include "strategy/clock/sim_clock.h"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using bpt::strategy::clock::SimClock;

class SimClockTest : public ::testing::Test {
protected:
    void SetUp() override { SimClock::reset(); }
    void TearDown() override { SimClock::reset(); }
};

TEST_F(SimClockTest, FallsBackToWallClockWhenNotActive) {
    EXPECT_FALSE(SimClock::active());
    const uint64_t a = SimClock::now_ns();
    const uint64_t b = SimClock::now_ns();
    // Wall clock advances; both reads should be plausible epoch-ns.
    EXPECT_GT(a, 1'000'000'000'000'000'000ULL);
    EXPECT_GE(b, a);
}

TEST_F(SimClockTest, SetNowNsActivatesAndReturnsExactValue) {
    constexpr uint64_t kSimTs = 1'767'225'600'000'000'000ULL;  // 2026-01-01T00:00:00Z
    SimClock::set_now_ns(kSimTs);
    EXPECT_TRUE(SimClock::active());
    EXPECT_EQ(SimClock::now_ns(), kSimTs);

    // Wall clock advances under us; SimClock must remain frozen at the
    // last set_now_ns until something updates it.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(SimClock::now_ns(), kSimTs);
}

TEST_F(SimClockTest, MultipleSetsTakeMostRecentValue) {
    SimClock::set_now_ns(100);
    SimClock::set_now_ns(200);
    SimClock::set_now_ns(50);  // sim ts can go backwards if a window starts earlier
    EXPECT_EQ(SimClock::now_ns(), 50ULL);
}

TEST_F(SimClockTest, ResetReturnsToWallClockMode) {
    SimClock::set_now_ns(123456789ULL);
    EXPECT_TRUE(SimClock::active());
    SimClock::reset();
    EXPECT_FALSE(SimClock::active());
    // Read after reset: should be a real wall-clock value, not 123456789.
    EXPECT_GT(SimClock::now_ns(), 1'000'000'000'000'000'000ULL);
}

TEST_F(SimClockTest, ConcurrentReadersSeeConsistentValue) {
    constexpr uint64_t kSimTs = 42'000'000'000ULL;
    SimClock::set_now_ns(kSimTs);

    constexpr int kThreads = 8;
    constexpr int kIters = 100'000;
    std::vector<std::thread> ts;
    std::atomic<bool> ok{true};
    for (int i = 0; i < kThreads; ++i) {
        ts.emplace_back([&]() {
            for (int j = 0; j < kIters; ++j) {
                if (SimClock::now_ns() != kSimTs) {
                    ok.store(false);
                    return;
                }
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_TRUE(ok.load());
}

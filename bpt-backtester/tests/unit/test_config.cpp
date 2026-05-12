// Unit tests for bpt::backtester::config::load()
#include "backtester/config/settings.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;

static fs::path write_toml(const std::string& content) {
    auto path = fs::temp_directory_path() / "backtester_test_config.toml";
    std::ofstream f(path);
    f << content;
    return path;
}

TEST(BacktesterConfigTest, ParsesFullConfig) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]
start = "2026-01-01T00:00:00Z"
end   = "2026-01-31T23:59:59Z"
allow_partial_data = false

[simulation.latency]
cex_base_ms            = 2
hyperliquid_base_ms    = 200
hyperliquid_jitter_ms  = 50

[data]
local_cache = "/opt/bpt/data/backtest-cache"

[aeron]
media_driver_dir = "/tmp/aeron"

[aeron.backtest_ack]
stream_id = 9001

[aeron.backtest_control]
stream_id = 9002

[[instruments]]
exchange = "BINANCE"
symbol   = "BTCUSDT"

[[instruments]]
exchange = "OKX"
symbol   = "BTC-USDT-SWAP"

[logging]
level = "debug"
dir   = "logs"

[metrics]
port = 9105
)");

    auto s = bpt::backtester::config::load(path.string());

    EXPECT_EQ(s.simulation.start, "2026-01-01T00:00:00Z");
    EXPECT_EQ(s.simulation.end, "2026-01-31T23:59:59Z");
    EXPECT_FALSE(s.simulation.allow_partial_data);
    // Legacy ms fields parse forward into per-venue submit_to_match_base_ns.
    EXPECT_EQ(s.simulation.latency.per_venue["OKX"].submit_to_match_base_ns,
              2 * 1'000'000ULL);
    EXPECT_EQ(s.simulation.latency.per_venue["HYPERLIQUID"].submit_to_match_base_ns,
              200 * 1'000'000ULL);
    EXPECT_EQ(s.simulation.latency.per_venue["HYPERLIQUID"].submit_to_match_jitter_ns,
              50 * 1'000'000ULL);

    EXPECT_EQ(s.data.local_cache, "/opt/bpt/data/backtest-cache");

    EXPECT_EQ(s.base.media_driver_dir, "/tmp/aeron");
    EXPECT_EQ(s.aeron.backtest_ack.stream_id, 9001);
    EXPECT_EQ(s.aeron.backtest_control.stream_id, 9002);

    ASSERT_EQ(s.instruments.size(), 2u);
    EXPECT_EQ(s.instruments[0].exchange, "BINANCE");
    EXPECT_EQ(s.instruments[0].symbol, "BTCUSDT");
    EXPECT_EQ(s.instruments[1].exchange, "OKX");
    EXPECT_EQ(s.instruments[1].symbol, "BTC-USDT-SWAP");

    EXPECT_EQ(s.base.logging.level, "debug");
}

TEST(BacktesterConfigTest, DefaultsAppliedWhenFieldsOmitted) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]
start = "2026-01-01T00:00:00Z"
end   = "2026-01-31T23:59:59Z"
)");

    auto s = bpt::backtester::config::load(path.string());

    EXPECT_FALSE(s.simulation.allow_partial_data);
    // No [simulation.latency] section — per_venue map is empty, defaults apply.
    EXPECT_TRUE(s.simulation.latency.per_venue.empty());
    EXPECT_EQ(s.simulation.latency.default_spec.submit_to_match_base_ns, 0u);
    EXPECT_EQ(s.data.local_cache, "/opt/bpt/data/backtest-cache");
    EXPECT_EQ(s.aeron.backtest_ack.stream_id, 9001);
    EXPECT_EQ(s.aeron.backtest_control.stream_id, 9002);
    EXPECT_EQ(s.endpoints.binance_md_port, 9100u);
    EXPECT_EQ(s.endpoints.okx_md_port, 9101u);
    EXPECT_EQ(s.endpoints.hyperliquid_md_port, 9102u);
    EXPECT_EQ(s.endpoints.deribit_md_port, 9103u);
    EXPECT_EQ(s.endpoints.binance_order_port, 9110u);
    EXPECT_EQ(s.endpoints.okx_order_port, 9111u);
    EXPECT_EQ(s.endpoints.hyperliquid_order_port, 9112u);
    EXPECT_EQ(s.endpoints.deribit_order_port, 9113u);
    EXPECT_EQ(s.base.logging.level, "info");
    EXPECT_TRUE(s.instruments.empty());
}

TEST(BacktesterConfigTest, MissingFileThrows) {
    EXPECT_THROW(bpt::backtester::config::load("/nonexistent/backtester.toml"), std::exception);
}

// ── Multi-window schema (Phase 2) ────────────────────────────────────────────

TEST(BacktesterConfigTest, SingleWindowAutoPopulatesWindowsList) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]
start = "2026-01-01T00:00:00Z"
end   = "2026-01-01T23:59:59Z"
)");
    auto s = bpt::backtester::config::load(path.string());
    ASSERT_EQ(s.simulation.windows.size(), 1u);
    EXPECT_EQ(s.simulation.windows[0].start, "2026-01-01T00:00:00Z");
    EXPECT_EQ(s.simulation.windows[0].end,   "2026-01-01T23:59:59Z");
    EXPECT_EQ(s.simulation.start, "2026-01-01T00:00:00Z");
    EXPECT_EQ(s.simulation.end,   "2026-01-01T23:59:59Z");
}

TEST(BacktesterConfigTest, ParsesMultipleWindows) {
    // Intentionally out of order to verify the loader sorts by start.
    auto path = write_toml(R"(
environment = "dev"
[simulation]

[[simulation.windows]]
start = "2026-01-03T13:00:00Z"
end   = "2026-01-03T14:00:00Z"

[[simulation.windows]]
start = "2026-01-01T13:00:00Z"
end   = "2026-01-01T14:00:00Z"
)");
    auto s = bpt::backtester::config::load(path.string());
    ASSERT_EQ(s.simulation.windows.size(), 2u);
    EXPECT_EQ(s.simulation.windows[0].start, "2026-01-01T13:00:00Z");
    EXPECT_EQ(s.simulation.windows[1].start, "2026-01-03T13:00:00Z");
    EXPECT_EQ(s.simulation.start, "2026-01-01T13:00:00Z");  // span min
    EXPECT_EQ(s.simulation.end,   "2026-01-03T14:00:00Z");  // span max
}

TEST(BacktesterConfigTest, RejectsBothTopLevelAndWindowsArray) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]
start = "2026-01-01T00:00:00Z"
end   = "2026-01-01T23:59:59Z"

[[simulation.windows]]
start = "2026-01-02T13:00:00Z"
end   = "2026-01-02T14:00:00Z"
)");
    EXPECT_THROW(bpt::backtester::config::load(path.string()), std::runtime_error);
}

TEST(BacktesterConfigTest, RejectsMissingWindowConfig) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]
allow_partial_data = true
)");
    EXPECT_THROW(bpt::backtester::config::load(path.string()), std::runtime_error);
}

TEST(BacktesterConfigTest, SessionsExpandIntoWindows) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]

[[simulation.sessions]]
name  = "us_open"
dates = ["2026-05-08", "2026-05-09"]
)");
    auto s = bpt::backtester::config::load(path.string());
    ASSERT_EQ(s.simulation.windows.size(), 2u);
    EXPECT_EQ(s.simulation.windows[0].start, "2026-05-08T13:00:00Z");
    EXPECT_EQ(s.simulation.windows[0].end,   "2026-05-08T15:00:00Z");
    EXPECT_EQ(s.simulation.windows[1].start, "2026-05-09T13:00:00Z");
    EXPECT_EQ(s.simulation.windows[1].end,   "2026-05-09T15:00:00Z");
}

TEST(BacktesterConfigTest, RejectsMixingSessionsWithOtherForms) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]
start = "2026-05-08T00:00:00Z"
end   = "2026-05-08T23:59:59Z"

[[simulation.sessions]]
name  = "us_open"
dates = ["2026-05-08"]
)");
    EXPECT_THROW(bpt::backtester::config::load(path.string()), std::runtime_error);
}

// ── Latency schema (Phase 3) ─────────────────────────────────────────────────

TEST(BacktesterConfigTest, ParsesLatencyPerVenue) {
    auto path = write_toml(R"(
environment = "dev"
[simulation]
start = "2026-05-08T00:00:00Z"
end   = "2026-05-08T23:59:59Z"

[simulation.latency]
seed = 42

[simulation.latency.default]
submit_to_match_base_ns = 1000000

[simulation.latency.HYPERLIQUID]
submit_to_match_base_ns   = 200000000
submit_to_match_jitter_ns = 50000000
match_to_report_base_ns   = 200000000

[simulation.latency.OKX]
submit_to_match_base_ns   = 2000000
submit_to_match_jitter_ns = 1000000
)");
    auto s = bpt::backtester::config::load(path.string());
    EXPECT_EQ(s.simulation.latency.seed, 42u);
    EXPECT_EQ(s.simulation.latency.default_spec.submit_to_match_base_ns, 1'000'000u);

    auto& hl = s.simulation.latency.per_venue["HYPERLIQUID"];
    EXPECT_EQ(hl.submit_to_match_base_ns,   200'000'000u);
    EXPECT_EQ(hl.submit_to_match_jitter_ns,  50'000'000u);
    EXPECT_EQ(hl.match_to_report_base_ns,   200'000'000u);
    EXPECT_EQ(hl.match_to_report_jitter_ns, 0u);

    auto& okx = s.simulation.latency.per_venue["OKX"];
    EXPECT_EQ(okx.submit_to_match_base_ns,    2'000'000u);
    EXPECT_EQ(okx.submit_to_match_jitter_ns,  1'000'000u);
}

TEST(BacktesterConfigTest, LegacyLatencyFieldsTranslateForward) {
    // Pre-Phase-3 configs used cex_base_ms / hyperliquid_base_ms / hyperliquid_jitter_ms.
    // The loader migrates these onto the new submit_to_match leg.
    auto path = write_toml(R"(
environment = "dev"
[simulation]
start = "2026-05-08T00:00:00Z"
end   = "2026-05-08T23:59:59Z"

[simulation.latency]
cex_base_ms           = 2
hyperliquid_base_ms   = 200
hyperliquid_jitter_ms = 50
)");
    auto s = bpt::backtester::config::load(path.string());
    EXPECT_EQ(s.simulation.latency.per_venue["BINANCE"].submit_to_match_base_ns,
              2 * 1'000'000ULL);
    EXPECT_EQ(s.simulation.latency.per_venue["OKX"].submit_to_match_base_ns,
              2 * 1'000'000ULL);
    EXPECT_EQ(s.simulation.latency.per_venue["HYPERLIQUID"].submit_to_match_base_ns,
              200 * 1'000'000ULL);
    EXPECT_EQ(s.simulation.latency.per_venue["HYPERLIQUID"].submit_to_match_jitter_ns,
              50 * 1'000'000ULL);
}

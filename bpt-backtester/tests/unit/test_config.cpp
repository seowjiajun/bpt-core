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
    EXPECT_EQ(s.simulation.latency.cex_base_ms, 2u);
    EXPECT_EQ(s.simulation.latency.hyperliquid_base_ms, 200u);
    EXPECT_EQ(s.simulation.latency.hyperliquid_jitter_ms, 50u);

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
[simulation]
start = "2026-01-01T00:00:00Z"
end   = "2026-01-31T23:59:59Z"
)");

    auto s = bpt::backtester::config::load(path.string());

    EXPECT_FALSE(s.simulation.allow_partial_data);
    EXPECT_EQ(s.simulation.latency.cex_base_ms, 2u);
    EXPECT_EQ(s.simulation.latency.hyperliquid_base_ms, 200u);
    EXPECT_EQ(s.simulation.latency.hyperliquid_jitter_ms, 50u);
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

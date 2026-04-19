// Unit tests for bpt::order_gateway::config::load() — no network, no Aeron.
#include "order_gateway/config/settings.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

// ── Helpers ───────────────────────────────────────────────────────────────────

static fs::path write_toml(const std::string& content) {
    auto path = fs::temp_directory_path() / "ogw_test_config.toml";
    std::ofstream f(path);
    f << content;
    return path;
}

// ── Full config ───────────────────────────────────────────────────────────────

TEST(GatewayConfigTest, ParsesFullConfig) {
    auto path = write_toml(R"(
environment = "qa"
exchanges   = ["OKX"]

[aeron]
media_driver_dir = "/tmp/aeron"

[aeron.order]
channel   = "aeron:ipc"
stream_id = 3001

[aeron.exec_report]
channel   = "aeron:ipc"
stream_id = 3002

[aeron.heartbeat]
channel   = "aeron:ipc"
stream_id = 3003

[gateway]
heartbeat_interval_ms  = 2000
stale_order_timeout_ms = 60000

[gateway.risk]
trading_enabled            = false
max_order_size_usd         = 2500.0
max_notional_per_order_usd = 10000.0
max_open_orders_per_venue  = 20
max_orders_per_second      = 5

[[adapters]]
exchange = "OKX"
testnet  = true
ws_host  = "wseeapap.okx.com"
ws_port  = "8443"
ws_path  = "/ws/v5/private"
use_tls  = true

[metrics]
port = 9103
)");

    auto s = bpt::order_gateway::config::load(path.string());

    EXPECT_EQ(s.base.environment, "qa");
    ASSERT_EQ(s.exchanges.size(), 1u);
    EXPECT_EQ(s.exchanges[0], "OKX");

    EXPECT_EQ(s.base.media_driver_dir, "/tmp/aeron");
    EXPECT_EQ(s.aeron.order.stream_id, 3001);
    EXPECT_EQ(s.aeron.exec_report.stream_id, 3002);
    EXPECT_EQ(s.aeron.heartbeat.stream_id, 3003);
    EXPECT_EQ(s.aeron.order.channel, "aeron:ipc");

    EXPECT_EQ(s.gateway.heartbeat_interval_ms, 2000u);
    EXPECT_EQ(s.gateway.stale_order_timeout_ms, 60000u);

    EXPECT_FALSE(s.gateway.risk.trading_enabled);
    EXPECT_DOUBLE_EQ(s.gateway.risk.max_order_size_usd, 2500.0);
    EXPECT_DOUBLE_EQ(s.gateway.risk.max_notional_per_order_usd, 10000.0);
    EXPECT_EQ(s.gateway.risk.max_open_orders_per_venue, 20u);
    EXPECT_EQ(s.gateway.risk.max_orders_per_second, 5u);

    ASSERT_EQ(s.gateway.adapters.size(), 1u);
    const auto& a = s.gateway.adapters[0];
    EXPECT_EQ(a.exchange, "OKX");
    EXPECT_TRUE(a.testnet);
    EXPECT_EQ(a.ws_host, "wseeapap.okx.com");
    EXPECT_EQ(a.ws_port, "8443");
    EXPECT_EQ(a.ws_path, "/ws/v5/private");
    EXPECT_TRUE(a.use_tls);

    EXPECT_EQ(s.base.metrics_port, 9103u);
}

// ── Defaults ──────────────────────────────────────────────────────────────────

TEST(GatewayConfigTest, DefaultsAppliedWhenFieldsOmitted) {
    // Minimal config — just enough for the loader not to throw.
    auto path = write_toml(R"(
exchanges = ["OKX"]

[[adapters]]
exchange = "OKX"
)");

    auto s = bpt::order_gateway::config::load(path.string());

    EXPECT_TRUE(s.base.environment.empty());
    EXPECT_EQ(s.aeron.order.stream_id, 3001);
    EXPECT_EQ(s.aeron.exec_report.stream_id, 3002);
    EXPECT_EQ(s.aeron.heartbeat.stream_id, 3003);
    EXPECT_EQ(s.aeron.order.channel, "aeron:ipc");

    EXPECT_EQ(s.gateway.heartbeat_interval_ms, 1000u);
    EXPECT_EQ(s.gateway.stale_order_timeout_ms, 30000u);
    EXPECT_TRUE(s.gateway.risk.trading_enabled);
    EXPECT_DOUBLE_EQ(s.gateway.risk.max_order_size_usd, 1000.0);
    // metrics_port default is now 0 (disabled) in BaseSettings; the old
    // 9103 was an order-gateway-specific default. Explicitly set via
    // [metrics].port = N in TOML when an exposer is wanted.
    EXPECT_EQ(s.base.metrics_port, 0u);

    ASSERT_EQ(s.gateway.adapters.size(), 1u);
    EXPECT_FALSE(s.gateway.adapters[0].testnet);
    EXPECT_TRUE(s.gateway.adapters[0].use_tls);
    EXPECT_EQ(s.gateway.adapters[0].rest_port, "443");
    EXPECT_EQ(s.gateway.adapters[0].ws_port, "443");
}

// ── Exchange filter ───────────────────────────────────────────────────────────

TEST(GatewayConfigTest, ExchangeFilterExcludesUnlistedAdapters) {
    auto path = write_toml(R"(
exchanges = ["OKX"]

[[adapters]]
exchange = "OKX"
ws_host  = "wseeapap.okx.com"

[[adapters]]
exchange = "BINANCE"
ws_host  = "stream.binance.com"
)");

    auto s = bpt::order_gateway::config::load(path.string());

    ASSERT_EQ(s.gateway.adapters.size(), 1u);
    EXPECT_EQ(s.gateway.adapters[0].exchange, "OKX");
}

TEST(GatewayConfigTest, MultipleExchangesLoaded) {
    auto path = write_toml(R"(
exchanges = ["OKX", "BINANCE"]

[[adapters]]
exchange = "OKX"

[[adapters]]
exchange = "BINANCE"

[[adapters]]
exchange = "HYPERLIQUID"
)");

    auto s = bpt::order_gateway::config::load(path.string());

    ASSERT_EQ(s.gateway.adapters.size(), 2u);
    std::vector<std::string> loaded;
    for (const auto& a : s.gateway.adapters)
        loaded.push_back(a.exchange);
    EXPECT_TRUE(std::find(loaded.begin(), loaded.end(), "OKX") != loaded.end());
    EXPECT_TRUE(std::find(loaded.begin(), loaded.end(), "BINANCE") != loaded.end());
}

TEST(GatewayConfigTest, EmptyExchangeFilterLoadsNoAdapters) {
    // No `exchanges` key → exchange_filter is empty → count("OKX") == 0 → no adapters loaded.
    auto path = write_toml(R"(
[[adapters]]
exchange = "OKX"
)");

    auto s = bpt::order_gateway::config::load(path.string());

    EXPECT_TRUE(s.gateway.adapters.empty());
}

// ── Adapter fields ────────────────────────────────────────────────────────────

TEST(GatewayConfigTest, AdapterRestHostAndPortParsed) {
    auto path = write_toml(R"(
exchanges = ["BINANCE"]

[[adapters]]
exchange  = "BINANCE"
rest_host = "api.binance.com"
rest_port = "443"
ws_host   = "stream.binance.com"
ws_port   = "9443"
use_tls   = true
)");

    auto s = bpt::order_gateway::config::load(path.string());

    ASSERT_EQ(s.gateway.adapters.size(), 1u);
    const auto& a = s.gateway.adapters[0];
    EXPECT_EQ(a.rest_host, "api.binance.com");
    EXPECT_EQ(a.rest_port, "443");
    EXPECT_EQ(a.ws_host, "stream.binance.com");
    EXPECT_EQ(a.ws_port, "9443");
    EXPECT_TRUE(a.use_tls);
}

// ── Custom stream IDs ─────────────────────────────────────────────────────────

TEST(GatewayConfigTest, CustomAeronStreamIds) {
    auto path = write_toml(R"(
exchanges = []

[aeron.order]
stream_id = 5001

[aeron.exec_report]
stream_id = 5002

[aeron.heartbeat]
stream_id = 5003
)");

    auto s = bpt::order_gateway::config::load(path.string());

    EXPECT_EQ(s.aeron.order.stream_id, 5001);
    EXPECT_EQ(s.aeron.exec_report.stream_id, 5002);
    EXPECT_EQ(s.aeron.heartbeat.stream_id, 5003);
}

// ── Error handling ────────────────────────────────────────────────────────────

TEST(GatewayConfigTest, MissingFileThrows) {
    EXPECT_THROW(bpt::order_gateway::config::load("/nonexistent/path/order-gateway.toml"), std::exception);
}

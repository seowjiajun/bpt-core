#include "refdata/config/settings.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

class ConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::ofstream out("test_config_bpt-refdata.toml");
        out << R"(
instrument_poll_interval_s = 7200

[aeron]
media_driver_dir = "/dev/shm/aeron"

[aeron.snapshot]
channel = "aeron:ipc"
stream_id = 1001

[aeron.delta]
channel = "aeron:ipc"
stream_id = 1002

[aeron.control]
channel = "aeron:ipc"
stream_id = 1003

[aeron.fee_schedule]
channel = "aeron:ipc"
stream_id = 1004

[aeron.refdata_status]
channel = "aeron:ipc"
stream_id = 1006

[[adapters]]
exchange = "BINANCE"
enabled = true
rest_host = "api.binance.com"
rest_port = "443"
ws_host = "fstream.binance.com"
ws_port = "443"
use_tls = true

[[adapters]]
exchange = "OKX"
enabled = false
rest_host = ""
rest_port = "443"
ws_port = "8443"

[[adapters]]
exchange = "HYPERLIQUID"
enabled = true
rest_host = "api.hyperliquid.xyz"
ws_host = "api.hyperliquid.xyz"
use_tls = true
)";
        out.close();
    }

    void TearDown() override { std::filesystem::remove("test_config_bpt-refdata.toml"); }
};

TEST_F(ConfigTest, ParsesAeronConfig) {
    auto s = bpt::refdata::config::load("test_config_bpt-refdata.toml");
    EXPECT_EQ(s.base.media_driver_dir, "/dev/shm/aeron");
    EXPECT_EQ(s.snapshot.stream_id, 1001);
    EXPECT_EQ(s.delta.stream_id, 1002);
    EXPECT_EQ(s.control.stream_id, 1003);
    EXPECT_EQ(s.fee_schedule.stream_id, 1004);
    EXPECT_EQ(s.refdata_status.stream_id, 1006);
}

TEST_F(ConfigTest, ParsesAdapters) {
    auto s = bpt::refdata::config::load("test_config_bpt-refdata.toml");
    ASSERT_EQ(s.adapters.size(), 3u);

    EXPECT_EQ(s.adapters[0].exchange, "BINANCE");
    EXPECT_TRUE(s.adapters[0].enabled);
    EXPECT_EQ(s.adapters[0].rest_host, "api.binance.com");
    EXPECT_TRUE(s.adapters[0].use_tls);

    EXPECT_EQ(s.adapters[1].exchange, "OKX");
    EXPECT_FALSE(s.adapters[1].enabled);
    EXPECT_EQ(s.adapters[1].ws_port, "8443");

    EXPECT_EQ(s.adapters[2].exchange, "HYPERLIQUID");
    EXPECT_TRUE(s.adapters[2].enabled);
}

TEST_F(ConfigTest, ParsesInstrumentPollInterval) {
    auto s = bpt::refdata::config::load("test_config_bpt-refdata.toml");
    EXPECT_EQ(s.instrument_poll_interval_s, 7200u);
}

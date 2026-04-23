#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <bpt_app/base_settings.h>
#include <bpt_common/aeron/stream_config.h>

namespace bpt::book::config {

struct AeronConfig {
    bpt::common::config::StreamConfig balance_snapshot{"aeron:ipc", 6001};
};

// Per-venue adapter config. Only the minimum needed for account queries;
// intentionally no trading fields (bpt-book is read-only and has no
// business placing orders).
struct AdapterConfig {
    std::string exchange;           // "HYPERLIQUID", "OKX", "BINANCE", "DERIBIT"
    std::string secret_name;        // systemd-creds name; may be empty for venues where account state is a public endpoint (HL)
    bool testnet{false};
    std::string rest_host;
    std::string rest_port{"443"};
    // HL-specific: wallet address is a public identifier and is carried
    // as a config value rather than a secret. Safe to log and commit.
    std::string wallet_address;
};

struct BookConfig {
    uint32_t poll_interval_ms{5000};
    std::vector<AdapterConfig> adapters;
};

struct Settings {
    bpt::app::BaseSettings base;
    std::vector<std::string> exchanges;   // filter from exchange_config
    AeronConfig aeron;
    BookConfig book;
};

Settings load(const std::string& path);

}  // namespace bpt::book::config

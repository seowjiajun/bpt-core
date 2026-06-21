#pragma once

/// @file
/// AeronConfig — strategy's typed, per-consumer projection of the shared
/// stream registry (deploy/config/aeron/streams.toml). Split out of
/// config.h so the messaging clients can take a slice by const-ref
/// (e.g. `const AeronConfig::Refdata&`) without pulling in toml++.

#include <bpt_common/aeron/stream_config.h>

namespace bpt::strategy::config {

struct AeronConfig {
    // media_driver_dir moved to BaseSettings; kept streams-only.
    //
    // Streams are grouped per consuming client, matching the AeronBus
    // build() boundary — `cfg.aeron.md` is exactly the slice the MD client
    // needs, nothing more. The client ctors take these sub-structs by
    // const-ref, so the wiring can't transpose two stream IDs. The IDs
    // themselves come from the shared registry, resolved by name in
    // config.cpp. Single-stream consumers stay flat — no cluster to group.
    using Stream = bpt::common::config::StreamConfig;

    // Reference data (required).
    struct Refdata {
        Stream control{"aeron:ipc", 1003};
        Stream snapshot{"aeron:ipc", 1001};
        Stream delta{"aeron:ipc", 1002};
        Stream fee_schedule{"aeron:ipc", 1004};  // FeeSchedule id=19
        Stream funding_rate{"aeron:ipc", 2005};  // FundingRate id=18
        Stream status{"aeron:ipc", 1006};        // RefDataReady id=16, RefDataError id=17
    } refdata;

    // Market data (optional — stream_id 0 means MD client is not started).
    struct Md {
        Stream control{"aeron:ipc", 0};
        Stream data{"aeron:ipc", 0};
        Stream ack_hb{"aeron:ipc", 0};
    } md;

    // Order gateway (optional — stream_id 0 means OrderGateway client is not started).
    struct Order {
        Stream submit{"aeron:ipc", 0};            // Strategy → OrderGateway
        Stream exec_report{"aeron:ipc", 0};       // OrderGateway → Strategy (ExecutionReport)
        Stream heartbeat{"aeron:ipc", 0};         // OrderGateway → Strategy (OrderGatewayHeartbeat)
        Stream account_snapshot{"aeron:ipc", 0};  // OrderGateway → Strategy (AccountSnapshot id=27)
    } order;

    // Pricer vol surface (optional — stream_id 0 means vol surface client is not started).
    struct Vol {
        Stream surface{"aeron:ipc", 0};        // Pricer → Strategy (VolSurface id=21)
        Stream pricer_status{"aeron:ipc", 0};  // PricerHeartbeat id=22, PricerReady id=23
    } vol;

    // Backtest (optional — only used when backtest_mode = true).
    struct Backtest {
        Stream control{"aeron:ipc", 9002};  // Backtester → Strategy (BacktestControl id=25)
        Stream ack{"aeron:ipc", 9001};      // Strategy → Backtester (BacktestAck id=24)
    } backtest;

    // Single-stream consumers — kept flat (nothing to group).
    Stream toxicity{"aeron:ipc", 0};            // Analytics → Strategy (ToxicityUpdate)
    Stream console_control{"aeron:ipc", 9003};  // bridge → Strategy
    Stream portfolio{"aeron:ipc", 9004};        // Strategy → bridge (~100ms)
};

}  // namespace bpt::strategy::config

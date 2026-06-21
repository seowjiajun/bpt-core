#pragma once

/// @file
/// AeronConfig — pricer's typed, per-producer projection of the shared
/// stream registry (deploy/config/aeron/streams.toml). Split out of
/// settings.h so the messaging clients can take a slice by const-ref
/// (e.g. `const AeronConfig::Refdata&`) instead of unpacking individual
/// channel + stream-id args.

#include <bpt_common/aeron/stream_config.h>

namespace bpt::pricer::config {

struct AeronConfig {
    using Stream = bpt::common::config::StreamConfig;

    // Market-data input (from md-gateway). data + control go to two
    // separate clients (MdSubscriber, MdSubscribeClient), so each takes a
    // single Stream — the slice just keeps the md-facing pair together.
    struct Md {
        Stream data{"aeron:ipc", 2002};     // md.feed — BBO/trade input
        Stream control{"aeron:ipc", 2001};  // md.control — our subscribe batch
    } md;

    // Reference-data input (from refdata). All three feed one client.
    struct Refdata {
        Stream snapshot{"aeron:ipc", 1001};
        Stream delta{"aeron:ipc", 1002};
        Stream control{"aeron:ipc", 1003};  // pricer → refdata subscription request
    } refdata;

    // Pricer's own outputs — single-stream, kept flat.
    Stream vol_surface{"aeron:ipc", 4001};    // VolSurface id=21
    Stream pricer_status{"aeron:ipc", 4002};  // PricerHeartbeat id=22, PricerReady id=23
};

}  // namespace bpt::pricer::config

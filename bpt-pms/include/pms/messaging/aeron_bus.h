#pragma once

/// @file
/// Bus boundary for bpt-book. Tiny — book has a single output stream
/// (the balance snapshot publisher) and no inbound subscriptions, so
/// the bus is mostly ceremony. Kept for symmetry with the rest of the
/// services and so `PmsService` doesn't take `<Aeron.h>` in its
/// constructor.

#include "pms/messaging/balance_snapshot_publisher.h"

#include <Aeron.h>

#include <memory>

namespace bpt::pms {
namespace config {
struct Settings;
}

namespace messaging {

struct PmsBus {
    std::unique_ptr<BalanceSnapshotPublisher> snapshot_pub;
};

class PmsAeronBus {
public:
    static PmsBus build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::pms

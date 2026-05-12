#pragma once

/// @file
/// Bus boundary for bpt-backtester. Mirrors the shape used by
/// bpt-refdata, bpt-md-gateway, bpt-order-gateway, bpt-strategy,
/// bpt-pricer, and bpt-analytics: every Aeron pub/sub the app needs
/// is built in one factory so `BacktesterApp` doesn't take
/// `<Aeron.h>` in its constructor.
///
/// Backtester only talks to Strategy through two streams: outbound
/// `BacktestControl` (tick command) and inbound `BacktestAck`. Tiny
/// surface but the same shape applies.

#include "backtester/messaging/backtest_ack_subscriber.h"
#include "backtester/messaging/backtest_control_publisher.h"

#include <Aeron.h>

#include <memory>

namespace bpt::backtester {
namespace config {
struct Settings;
}

namespace messaging {

struct BacktesterBus {
    std::unique_ptr<BacktestControlPublisher> ctrl_pub;
    std::unique_ptr<BacktestAckSubscriber> ack_sub;
};

class BacktesterAeronBus {
public:
    /// Build the two messaging objects the backtester needs to
    /// tick-gate Strategy. Sole place that calls into `<Aeron.h>` from
    /// the application layer.
    static BacktesterBus build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::backtester

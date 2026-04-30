#pragma once

/// @file
/// Bus boundary for bpt-book. Tiny — book has a single output stream
/// (the balance snapshot publisher) and no inbound subscriptions, so
/// the bus is mostly ceremony. Kept for symmetry with the rest of the
/// services and so `BookApp` doesn't take `<Aeron.h>` in its
/// constructor.

#include "book/messaging/balance_snapshot_publisher.h"

#include <Aeron.h>

#include <memory>

namespace bpt::book {
namespace config { struct Settings; }

namespace messaging {

struct BookBus {
    std::unique_ptr<BalanceSnapshotPublisher> snapshot_pub;
};

class BookAeronBus {
public:
    static BookBus build(std::shared_ptr<aeron::Aeron> aeron,
                         const config::Settings& settings);
};

}  // namespace messaging
}  // namespace bpt::book

#include "book/messaging/aeron_bus.h"

#include "book/config/settings.h"

namespace bpt::book::messaging {

BookBus BookAeronBus::build(std::shared_ptr<aeron::Aeron> aeron, const config::Settings& settings) {
    BookBus bus;
    bus.snapshot_pub = std::make_unique<BalanceSnapshotPublisher>(std::move(aeron),
                                                                  settings.aeron.balance_snapshot.channel,
                                                                  settings.aeron.balance_snapshot.stream_id);
    return bus;
}

}  // namespace bpt::book::messaging

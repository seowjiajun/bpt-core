#include "refdata/messaging/publisher.h"

#include <iostream>

namespace bpt::refdata::messaging {

RefdataPublisher::RefdataPublisher() {
    // Initialize Aeron publications here
}

RefdataPublisher::~RefdataPublisher() = default;

void RefdataPublisher::publishSnapshot(const std::vector<refdata::Instrument>& instruments) {
    bpt::common::log::info("Publishing snapshot of {} instruments", instruments.size());
    // Iterate and send via Aeron
}

void RefdataPublisher::publishDelta(const refdata::Instrument& instrument) {
    bpt::common::log::info("Publishing delta for instrument: {}", instrument.inst_uid);
    // Send via Aeron
}

}  // namespace bpt::refdata::messaging

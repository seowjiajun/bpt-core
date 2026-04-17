#pragma once

#include "refdata/messaging/messages.h"
#include "refdata/registry/instrument_registry.h"

#include <Aeron.h>

#include <memory>

namespace bpt::refdata::messaging {

class RefdataSnapshotPublisher {
public:
    RefdataSnapshotPublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(const registry::InstrumentRegistry& registry, const RefdataRequest& request, uint64_t seq_start);

private:
    std::shared_ptr<aeron::Publication> publication_;
};

}  // namespace bpt::refdata::messaging

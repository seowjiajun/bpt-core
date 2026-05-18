#pragma once

/// \file
/// \brief Outbound port: per-instrument delta + heartbeat publish.

#include <messages/DeltaUpdateType.h>

#include <cstdint>

namespace bpt::refdata::model {
struct Instrument;
}

namespace bpt::refdata::messaging::api {

class RefdataDeltaPublisher {
public:
    virtual ~RefdataDeltaPublisher() = default;

    virtual void publish_delta(bpt::messages::DeltaUpdateType::Value update_type, const model::Instrument& inst) = 0;

    virtual void publish_heartbeat() = 0;

    virtual uint64_t current_sequence() const = 0;
};

}  // namespace bpt::refdata::messaging::api

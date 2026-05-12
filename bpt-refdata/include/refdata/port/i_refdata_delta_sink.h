#pragma once

/// \file
/// \brief Outbound port: per-instrument delta + heartbeat sink.

#include <messages/DeltaUpdateType.h>

#include <cstdint>

namespace bpt::refdata::refdata {
struct Instrument;
}

namespace bpt::refdata::port {

class IRefdataDeltaSink {
public:
    virtual ~IRefdataDeltaSink() = default;

    virtual void publish_delta(bpt::messages::DeltaUpdateType::Value update_type, const refdata::Instrument& inst) = 0;

    virtual void publish_heartbeat() = 0;

    virtual uint64_t current_sequence() const = 0;
};

}  // namespace bpt::refdata::port

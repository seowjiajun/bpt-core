#pragma once

/// \file
/// \brief Outbound port: full registry snapshot publish in response to a control request.

#include <cstdint>

namespace bpt::refdata::registry {
class InstrumentRegistry;
}

namespace bpt::refdata::messaging {
struct RefdataRequest;
}

namespace bpt::refdata::port {

class IRefdataSnapshotSink {
public:
    virtual ~IRefdataSnapshotSink() = default;

    /// \brief Publish a snapshot of `registry` filtered by `request`, tagged with `seq_start`
    ///        so subscribers can join the delta stream without gaps.
    virtual void publish(const registry::InstrumentRegistry& registry,
                         const messaging::RefdataRequest& request,
                         uint64_t seq_start) = 0;
};

}  // namespace bpt::refdata::port

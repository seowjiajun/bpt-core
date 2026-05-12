#pragma once

/// @file
/// Publisher for the analytics toxicity stream. Wraps the generic
/// Aeron Publisher with a domain-typed `publish(ToxicityUpdate&)`
/// method so callers don't reach for `aeron::concurrent::AtomicBuffer`
/// at the call site.

#include "analytics/messaging/toxicity_update.h"

#include <bpt_common/aeron/publisher.h>
#include <memory>
#include <string>

namespace bpt::analytics::messaging {

class ToxicityPublisher {
public:
    ToxicityPublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    /// Returns true if Aeron accepted the offer. Idempotent/periodic
    /// stream — next tick replaces the previous, so drop-on-no-subscriber
    /// is fine. Policy is `kBoundedRetry` to match the inline behaviour
    /// the app used previously.
    bool publish(const ToxicityUpdate& update);

private:
    std::unique_ptr<bpt::common::aeron::Publisher> pub_;
};

}  // namespace bpt::analytics::messaging

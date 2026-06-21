#pragma once

/// @file
/// Aeron+POD implementation of api::ToxicityPublisher. Composes
/// PodToxicityCodec for serialisation. Wraps the generic Aeron
/// Publisher with a domain-typed `publish(ToxicityUpdate&)` so callers
/// don't reach for `::aeron::concurrent::AtomicBuffer` at the call site.

#include "analytics/messaging/codecs/pod_toxicity_codec.h"
#include "analytics/messaging/publishers/api/toxicity_publisher.h"

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/stream_config.h>
#include <memory>
#include <string>

namespace bpt::analytics::messaging::aeron {

class ToxicityPublisher : public api::ToxicityPublisher {
public:
    ToxicityPublisher(std::shared_ptr<::aeron::Aeron> aeron, const bpt::common::config::StreamConfig& stream);

    bool publish(const ToxicityUpdate& update) override;

private:
    std::unique_ptr<bpt::common::aeron::Publisher> pub_;
    PodToxicityCodec codec_;
};

}  // namespace bpt::analytics::messaging::aeron

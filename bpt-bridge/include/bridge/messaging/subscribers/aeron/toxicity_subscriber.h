#pragma once

/// @file
/// Aeron-backed bpt-analytics ToxicityUpdate subscriber.

#include "bridge/messaging/subscribers/api/toxicity_subscriber.h"

#include <Aeron.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging::aeron {

class ToxicitySubscriber final : public api::ToxicitySubscriber {
public:
    ToxicitySubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    int poll(int fragment_limit = 4) override;

private:
    std::shared_ptr<::aeron::Subscription> sub_;
};

}  // namespace bpt::bridge::messaging::aeron

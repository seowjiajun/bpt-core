#pragma once

/// @file
/// Aeron-backed ExecutionReport subscriber. Decodes SBE off the hot path
/// so BridgeService works in the domain types defined on
/// `api::ExecSubscriber`.

#include "bridge/messaging/subscribers/api/exec_subscriber.h"

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging::aeron {

class ExecSubscriber final : public api::ExecSubscriber {
public:
    ExecSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    int poll(int fragment_limit = 32) override;

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
};

}  // namespace bpt::bridge::messaging::aeron

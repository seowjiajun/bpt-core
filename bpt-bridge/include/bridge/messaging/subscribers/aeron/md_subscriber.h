#pragma once

/// @file
/// Aeron-backed MdGateway MD-data subscriber. Thin wrapper around the
/// Aeron Subscriber — all decoding happens inline in the .cpp.

#include "bridge/messaging/subscribers/api/md_subscriber.h"

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging::aeron {

class MdSubscriber final : public api::MdSubscriber {
public:
    MdSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    int poll(int fragment_limit = 32) override;

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
};

}  // namespace bpt::bridge::messaging::aeron

#pragma once

/// @file
/// Aeron-backed AccountSnapshot subscriber (OrderGateway stream 3004).
/// Decoded balance / equity / open-position state surfaced through
/// `api::AccountSubscriber`'s handler.

#include "bridge/messaging/subscribers/api/account_subscriber.h"

#include <Aeron.h>

#include <bpt_common/aeron/subscriber.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::bridge::messaging::aeron {

class AccountSubscriber final : public api::AccountSubscriber {
public:
    AccountSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id);

    int poll(int fragment_limit = 8) override;

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
};

}  // namespace bpt::bridge::messaging::aeron

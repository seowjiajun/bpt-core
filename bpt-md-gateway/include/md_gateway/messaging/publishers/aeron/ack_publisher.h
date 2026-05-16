#pragma once

/// \file
/// \brief Aeron-backed concrete implementation of api::AckPublisher.
///
/// Emits MdSubscriptionAck, MdSubscriptionHeartbeat, and MdServiceHeartbeat
/// on the gateway → strategy ack/heartbeat stream (default 2003). All
/// three message types share this publisher because they're small,
/// low-frequency notifications travelling on the same Aeron stream.

#include "md_gateway/messaging/codecs/sbe_md_service_heartbeat_codec.h"
#include "md_gateway/messaging/codecs/sbe_md_subscription_ack_codec.h"
#include "md_gateway/messaging/codecs/sbe_md_subscription_heartbeat_codec.h"
#include "md_gateway/messaging/publishers/api/ack_publisher.h"

#include <Aeron.h>

#include <messages/AckStatus.h>

#include <atomic>
#include <bpt_common/aeron/publisher.h>
#include <cstdint>
#include <memory>
#include <string>

namespace bpt::md_gateway::messaging::aeron {

/// \brief Aeron implementation of the ACK / heartbeat outbound port.
///
/// Called from both the main poll thread (acks emitted after subscription
/// processing) and the service-heartbeat timer. Concurrency is safe via
/// the same aeron::Publication thread-safety contract used by MdPublisher.
class AckPublisher final : public api::AckPublisher {
public:
    AckPublisher(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish_ack(uint64_t correlation_id,
                     uint64_t instrument_id,
                     const char* exchange,
                     bpt::messages::AckStatus::Value status) override;

    void publish_subscription_heartbeat(uint64_t instrument_id) override;

    void publish_service_heartbeat() override;

    /// \brief Monotonic counter of frames emitted on this stream — used by tests.
    [[nodiscard]] uint64_t current_seq() const { return seq_.load(std::memory_order_relaxed); }

private:
    bpt::common::aeron::Publisher       publisher_;
    SbeMdSubscriptionAckCodec           ack_codec_;
    SbeMdSubscriptionHeartbeatCodec     sub_hb_codec_;
    SbeMdServiceHeartbeatCodec          svc_hb_codec_;
    std::atomic<uint64_t>               seq_{0};
};

}  // namespace bpt::md_gateway::messaging::aeron

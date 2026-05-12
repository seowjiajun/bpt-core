#pragma once

/// @file
/// AeronMdClient — Aeron-backed implementation of IMdClient. Publishes
/// MdSubscribeBatch on stream 2001, subscribes to MdMarketData + MdTrade
/// on stream 2002, and subscribes to acks + heartbeats on stream 2003.
///
/// `MdClient` remains as a deprecated alias for AeronMdClient so existing
/// call sites compile; new code should depend on IMdClient and have
/// AeronMdClient injected via the bus factory.
///
/// Single-threaded: only call from the strategy poll thread.

#include "strategy/md/i_md_client.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/AckStatus.h>
#include <messages/TradeSide.h>

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/subscriber.h>
#include <memory>
#include <string>
#include <utility>

namespace bpt::strategy::md {

class AeronMdClient : public IMdClient {
public:
    AeronMdClient(std::shared_ptr<aeron::Aeron> aeron,
                  const std::string& channel,
                  int control_stream,
                  int data_stream,
                  int ack_hb_stream);

    void subscribe(uint64_t correlation_id, const std::vector<InstrumentDesc>& instruments) override;

    int poll(int fragment_limit = 10) override;

    /// Nanosecond timestamp of the last MdServiceHeartbeat (0 if none yet).
    /// Not part of IMdClient — strategy doesn't read it; kept here for
    /// AeronMdClient's own diagnostics + any future heartbeat-watchdog wiring.
    [[nodiscard]] uint64_t last_service_heartbeat_ns() const { return last_service_hb_ns_; }

private:
    void handle_data_fragment(aeron::AtomicBuffer& buffer,
                              aeron::util::index_t offset,
                              aeron::util::index_t length,
                              aeron::Header& header);

    void handle_ack_hb_fragment(aeron::AtomicBuffer& buffer,
                                aeron::util::index_t offset,
                                aeron::util::index_t length,
                                aeron::Header& header);

    std::unique_ptr<bpt::common::aeron::Publisher> ctrl_pub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> data_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> ack_hb_sub_;
    uint64_t last_service_hb_ns_{0};
};

/// Backward-compat alias. New code should depend on IMdClient (interface)
/// and accept AeronMdClient by injection. Existing call sites that use
/// `md::MdClient` continue to compile because AeronMdClient's
/// InstrumentDesc + callback typedefs are inherited from IMdClient.
using MdClient = AeronMdClient;

}  // namespace bpt::strategy::md

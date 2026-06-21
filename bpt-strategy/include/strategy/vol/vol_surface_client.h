#pragma once

/// @file
/// VolSurfaceClient — subscribes to Pricer's VolSurface (stream 4001) and
/// status (stream 4002). Templated on the Handler type that receives
/// decoded events — in prod the Handler is `StrategyService` and the
/// per-event dispatch path is direct (no `std::function` indirection).
///
/// Pure subscriber — no publications. Single-threaded: only call from
/// the strategy poll thread.

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/MessageHeader.h>
#include <messages/PricerHeartbeat.h>
#include <messages/PricerReady.h>
#include <messages/VolSurface.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <memory>
#include <strategy/config/aeron_config.h>
#include <string>
#include <utility>

namespace bpt::strategy::vol {

/// Empty abstract base — strategies that store a pointer to the vol
/// client (but don't dispatch into it) hold `IVolSurfaceClient*`. The
/// templated `VolSurfaceClient<H>` is what the bus owns + sets the
/// handler on.
class IVolSurfaceClient {
public:
    virtual ~IVolSurfaceClient() = default;
};

template <class Handler>
class VolSurfaceClient : public IVolSurfaceClient {
public:
    VolSurfaceClient(std::shared_ptr<aeron::Aeron> aeron, const config::AeronConfig::Vol& streams) {
        surface_sub_ =
            bpt::common::aeron::wait_for_subscription(aeron, streams.surface.channel, streams.surface.stream_id);
        status_sub_ = bpt::common::aeron::wait_for_subscription(aeron,
                                                                streams.pricer_status.channel,
                                                                streams.pricer_status.stream_id);

        // FragmentAssembler for surface stream — VolSurface messages can be large
        surface_assembler_ = std::make_unique<aeron::FragmentAssembler>(
            [this](aeron::AtomicBuffer& buffer,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& header) { handle_surface_fragment(buffer, offset, length, header); });

        bpt::common::log::info("[VolSurfaceClient] Subscriptions ready: surface={} status={}",
                               streams.surface.stream_id,
                               streams.pricer_status.stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 10) {
        int frags = 0;
        if (surface_sub_)
            frags += surface_sub_->poll(surface_assembler_->handler(), fragment_limit);
        if (status_sub_) {
            frags += status_sub_->poll(
                [this](aeron::AtomicBuffer& buffer,
                       aeron::util::index_t offset,
                       aeron::util::index_t length,
                       aeron::Header& header) { handle_status_fragment(buffer, offset, length, header); },
                fragment_limit);
        }
        return frags;
    }

    [[nodiscard]] uint64_t last_heartbeat_ns() const noexcept { return last_heartbeat_ns_; }

private:
    void handle_surface_fragment(aeron::AtomicBuffer& buffer,
                                 aeron::util::index_t offset,
                                 aeron::util::index_t length,
                                 aeron::Header& /*header*/) {
        using bpt::messages::MessageHeader;
        using bpt::messages::VolSurface;

        if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
            return;

        char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;

        MessageHeader hdr;
        hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

        if (hdr.templateId() != VolSurface::sbeTemplateId())
            return;

        VolSurface msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<uint64_t>(length));

        if (handler_ != nullptr) [[likely]]
            handler_->on_vol_surface(msg);
    }

    void handle_status_fragment(aeron::AtomicBuffer& buffer,
                                aeron::util::index_t offset,
                                aeron::util::index_t length,
                                aeron::Header& /*header*/) {
        using bpt::messages::MessageHeader;
        using bpt::messages::PricerHeartbeat;
        using bpt::messages::PricerReady;

        if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
            return;

        char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;

        MessageHeader hdr;
        hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

        const auto tid = hdr.templateId();

        if (tid == PricerHeartbeat::sbeTemplateId()) {
            PricerHeartbeat hb;
            hb.wrapForDecode(data,
                             MessageHeader::encodedLength(),
                             hdr.blockLength(),
                             hdr.version(),
                             static_cast<uint64_t>(length));
            last_heartbeat_ns_ = hb.timestampNs();

        } else if (tid == PricerReady::sbeTemplateId()) {
            PricerReady ready;
            ready.wrapForDecode(data,
                                MessageHeader::encodedLength(),
                                hdr.blockLength(),
                                hdr.version(),
                                static_cast<uint64_t>(length));

            bpt::common::log::info("[VolSurfaceClient] PricerReady: exchanges=0x{:02x} underlyings={} points={}",
                                   ready.exchangesLoaded(),
                                   ready.underlyingCount(),
                                   ready.pointCount());

            if (handler_ != nullptr) [[likely]]
                handler_->on_pricer_ready(ready.exchangesLoaded(), ready.underlyingCount(), ready.pointCount());
        }
    }

    std::shared_ptr<aeron::Subscription> surface_sub_;
    std::shared_ptr<aeron::Subscription> status_sub_;
    std::unique_ptr<aeron::FragmentAssembler> surface_assembler_;
    Handler* handler_{nullptr};
    uint64_t last_heartbeat_ns_{0};
};

}  // namespace bpt::strategy::vol

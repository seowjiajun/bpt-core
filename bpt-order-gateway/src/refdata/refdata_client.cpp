#include "order_gateway/refdata/refdata_client.h"

#include <messages/MessageHeader.h>
#include <messages/RefDataDelta.h>
#include <messages/RefDataSnapshot.h>
#include <messages/RefDataSubscriptionRequest.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <yggdrasil/aeron/aeron_utils.h>
#include <yggdrasil/logging.h>

namespace bpt::order_gateway::refdata {

RefdataClient::RefdataClient(std::shared_ptr<aeron::Aeron> aeron,
                             const std::string& channel,
                             int control_stream,
                             int snapshot_stream,
                             int delta_stream) {
    ctrl_pub_ = ygg::aeron::wait_for_publication(aeron, channel, control_stream);
    snap_sub_ = ygg::aeron::wait_for_subscription(aeron, channel, snapshot_stream);
    delta_sub_ = ygg::aeron::wait_for_subscription(aeron, channel, delta_stream);

    snap_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_snapshot_fragment(buf, offset, length, hdr);
        });

    delta_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_delta_fragment(buf, offset, length, hdr);
        });

    ygg::log::info("[Heimdall] RefdataClient connected: ctrl={} snap={} delta={}",
                   control_stream,
                   snapshot_stream,
                   delta_stream);
}

void RefdataClient::subscribe(uint64_t correlation_id, std::vector<CanonicalFilter> filters) {
    correlation_id_ = correlation_id;

    using namespace bpt::messages;
    const uint16_t nf = static_cast<uint16_t>(filters.size());

    std::size_t buf_size = MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength() +
                           RefDataSubscriptionRequest::Instruments::sbeHeaderSize() +
                           RefDataSubscriptionRequest::CanonicalFilter::sbeHeaderSize() +
                           nf * RefDataSubscriptionRequest::CanonicalFilter::sbeBlockLength();

    std::vector<char> buf(buf_size, '\0');

    RefDataSubscriptionRequest req;
    req.wrapAndApplyHeader(buf.data(), 0, buf_size)
        .correlationId(correlation_id)
        .timestampNs(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count()));

    req.instrumentsCount(0);

    auto& g = req.canonicalFilterCount(nf);
    for (const auto& f : filters) {
        auto& entry = g.next();
        entry.putBaseCurrency(f.base.c_str());
        entry.putQuoteCurrency(f.quote.c_str());
        entry.instrumentType(f.instrument_type);
        entry.putExchange(f.exchange.c_str());
    }

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf.data()), static_cast<aeron::util::index_t>(buf_size));
    while (ctrl_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(buf_size)) < 0) {
        std::this_thread::yield();
    }

    ygg::log::info(
        "[Heimdall] Refdata subscription request sent: correlation_id={} "
        "canonical_filters={}",
        correlation_id,
        nf);
}

void RefdataClient::handle_snapshot_fragment(aeron::AtomicBuffer& buffer,
                                             aeron::util::index_t offset,
                                             aeron::util::index_t length,
                                             aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() != RefDataSnapshot::sbeTemplateId())
        return;

    RefDataSnapshot msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<std::size_t>(length));

    if (msg.correlationId() != correlation_id_)
        return;

    cache_.apply_snapshot(msg);
    ygg::log::info("[Heimdall] Refdata snapshot received: {} instruments", cache_.size());

    if (on_snapshot_complete)
        on_snapshot_complete(cache_);
}

void RefdataClient::handle_delta_fragment(aeron::AtomicBuffer& buffer,
                                          aeron::util::index_t offset,
                                          aeron::util::index_t length,
                                          aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (!cache_.snapshot_received())
        return;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() != RefDataDelta::sbeTemplateId())
        return;

    RefDataDelta msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<std::size_t>(length));

    auto update_type = msg.updateType();

    if (update_type == bpt::messages::DeltaUpdateType::NULL_VALUE) {
        last_heartbeat_ns_ = msg.timestampNs();
        cache_.apply_delta(msg);
        return;
    }

    bool ok = cache_.apply_delta(msg);
    if (!ok) {
        ygg::log::warn(
            "[Heimdall] Delta sequence gap detected (last={} received={}), "
            "resetting cache",
            cache_.last_delta_seq(),
            msg.deltaSeqNum());
        cache_.reset();
        if (on_gap_detected)
            on_gap_detected();
        return;
    }

    if (on_delta) {
        if (auto inst = cache_.get(msg.instrumentId()))
            on_delta(*inst, update_type);
    }
}

int RefdataClient::poll(int fragment_limit) {
    int total = 0;
    total += snap_sub_->poll(snap_assembler_->handler(), fragment_limit);
    total += delta_sub_->poll(delta_assembler_->handler(), fragment_limit);
    return total;
}

}  // namespace bpt::order_gateway::refdata

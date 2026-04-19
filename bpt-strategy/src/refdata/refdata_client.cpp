#include "strategy/refdata/refdata_client.h"

#include <messages/FeeSchedule.h>
#include <messages/FundingRate.h>
#include <messages/MessageHeader.h>
#include <messages/RefDataDelta.h>
#include <messages/RefDataError.h>
#include <messages/RefDataReady.h>
#include <messages/RefDataSnapshot.h>
#include <messages/RefDataSubscriptionRequest.h>

#include <cstring>
#include <x86intrin.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::strategy::refdata {

RefdataClient::RefdataClient(std::shared_ptr<aeron::Aeron> aeron,
                             const std::string& channel,
                             int control_stream,
                             int snapshot_stream,
                             int delta_stream,
                             int fee_schedule_stream,
                             int funding_rate_stream,
                             int status_stream,
                             FeeCache& fee_cache,
                             FundingRateCache& funding_rate_cache)
    : fee_cache_(fee_cache),
      funding_rate_cache_(funding_rate_cache) {
    ctrl_pub_ = bpt::common::aeron::wait_for_publication(aeron, channel, control_stream);
    snap_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, snapshot_stream);
    delta_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, delta_stream);
    fee_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, fee_schedule_stream);
    funding_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, funding_rate_stream);
    status_sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, status_stream);

    snap_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_snapshot_fragment(buf, offset, length, hdr);
        });

    delta_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t offset, aeron::util::index_t length, aeron::Header& hdr) {
            handle_delta_fragment(buf, offset, length, hdr);
        });

    fee_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t off, aeron::util::index_t len, aeron::Header& hdr) {
            handle_fee_schedule_fragment(buf, off, len, hdr);
        });

    funding_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t off, aeron::util::index_t len, aeron::Header& hdr) {
            handle_funding_rate_fragment(buf, off, len, hdr);
        });

    status_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buf, aeron::util::index_t off, aeron::util::index_t len, aeron::Header& hdr) {
            handle_status_fragment(buf, off, len, hdr);
        });

    bpt::common::log::info("RefdataClient connected: ctrl={} snap={} delta={} fee={} funding={} status={}",
                   control_stream,
                   snapshot_stream,
                   delta_stream,
                   fee_schedule_stream,
                   funding_rate_stream,
                   status_stream);
}

void RefdataClient::subscribe(uint64_t correlation_id, std::vector<CanonicalFilter> filters) {
    correlation_id_ = correlation_id;

    using namespace bpt::messages;
    const uint16_t nf = static_cast<uint16_t>(filters.size());

    // instruments group sent empty; canonical filter group carries the actual filter.
    std::size_t buf_size = MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength() +
                           RefDataSubscriptionRequest::Instruments::sbeHeaderSize() +
                           RefDataSubscriptionRequest::CanonicalFilter::sbeHeaderSize() +
                           nf * RefDataSubscriptionRequest::CanonicalFilter::sbeBlockLength();

    std::vector<char> buf(buf_size, '\0');

    RefDataSubscriptionRequest req;
    req.wrapAndApplyHeader(buf.data(), 0, buf_size)
        .correlationId(correlation_id)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

    req.instrumentsCount(0);  // empty — filter is in canonicalFilter group

    auto& g = req.canonicalFilterCount(nf);
    for (const auto& f : filters) {
        auto& entry = g.next();
        entry.putBaseCurrency(f.base.c_str());
        entry.putQuoteCurrency(f.quote.c_str());
        entry.instrumentType(f.instrument_type);
        entry.putExchange(f.exchange.c_str());
    }

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf.data()), static_cast<aeron::util::index_t>(buf_size));
    while (ctrl_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(buf_size)) < 0)
        _mm_pause();

    bpt::common::log::info("Subscription request sent: correlation_id={} canonical_filters={}", correlation_id, nf);
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
    bpt::common::log::info("Snapshot received: {} instruments", cache_.size());

    if (on_snapshot_complete)
        on_snapshot_complete(cache_);
}

void RefdataClient::handle_delta_fragment(aeron::AtomicBuffer& buffer,
                                          aeron::util::index_t offset,
                                          aeron::util::index_t length,
                                          aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (!cache_.snapshot_received())
        return;  // discard deltas until snapshot is applied

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

    // Heartbeat: just update last-heartbeat timestamp; cache handles seq tracking.
    if (update_type == bpt::messages::DeltaUpdateType::NULL_VALUE) {
        last_heartbeat_ns_ = msg.timestampNs();
        cache_.apply_delta(msg);
        return;
    }

    bool ok = cache_.apply_delta(msg);
    if (!ok) {
        bpt::common::log::warn(
            "Delta sequence gap detected (last={} received={}), resetting cache — "
            "resubscription required",
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

void RefdataClient::handle_fee_schedule_fragment(aeron::AtomicBuffer& buffer,
                                                 aeron::util::index_t offset,
                                                 aeron::util::index_t length,
                                                 aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() != FeeSchedule::sbeTemplateId())
        return;

    FeeSchedule msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<std::size_t>(length));

    fee_cache_.update(msg.exchangeId(), msg.instrumentId(), msg.makerFeeBps(), msg.takerFeeBps(), msg.updatedTs());

    bpt::common::log::debug("[Strategy] FeeSchedule: exchange={} instrument={} maker={}bps taker={}bps",
                    ExchangeId::c_str(msg.exchangeId()),
                    msg.instrumentId(),
                    msg.makerFeeBps(),
                    msg.takerFeeBps());
}

void RefdataClient::handle_funding_rate_fragment(aeron::AtomicBuffer& buffer,
                                                 aeron::util::index_t offset,
                                                 aeron::util::index_t length,
                                                 aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() != FundingRate::sbeTemplateId())
        return;

    FundingRate msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<std::size_t>(length));

    funding_rate_cache_.update(msg.exchangeId(),
                               msg.instrumentId(),
                               msg.rateBps(),
                               msg.nextFundingTs(),
                               msg.collectedTs());

    bpt::common::log::debug("[Strategy] FundingRate: exchange={} instrument={} rate={}bps",
                    ExchangeId::c_str(msg.exchangeId()),
                    msg.instrumentId(),
                    msg.rateBps());
}

void RefdataClient::handle_status_fragment(aeron::AtomicBuffer& buffer,
                                           aeron::util::index_t offset,
                                           aeron::util::index_t length,
                                           aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (static_cast<std::size_t>(length) < MessageHeader::encodedLength())
        return;

    char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
    MessageHeader hdr(data, static_cast<std::size_t>(length));

    if (hdr.templateId() == RefDataReady::sbeTemplateId()) {
        RefDataReady msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));

        bpt::common::log::debug("[Strategy] RefDataReady: exchanges=0x{:02x} instruments={} fee_schedules={} funding_rates={}",
                        msg.exchangesLoaded(),
                        msg.instrumentCount(),
                        msg.feeSchedulesLoaded(),
                        msg.fundingRatesLoaded());

        if (on_ready)
            on_ready(msg.exchangesLoaded(),
                     msg.instrumentCount(),
                     msg.feeSchedulesLoaded() != 0,
                     msg.fundingRatesLoaded() != 0);

    } else if (hdr.templateId() == RefDataError::sbeTemplateId()) {
        RefDataError msg;
        msg.wrapForDecode(data,
                          MessageHeader::encodedLength(),
                          hdr.blockLength(),
                          hdr.version(),
                          static_cast<std::size_t>(length));

        bpt::common::log::error("[Strategy] RefDataError: type={} exchange={} instrument={}",
                        RefDataErrorType::c_str(msg.errorType()),
                        ExchangeId::c_str(msg.exchangeId()),
                        msg.instrumentId());

        if (on_error)
            on_error(msg.errorType(), msg.exchangeId(), msg.instrumentId());
    }
}

int RefdataClient::poll(int fragment_limit) {
    int total = 0;
    total += snap_sub_->poll(snap_assembler_->handler(), fragment_limit);
    total += delta_sub_->poll(delta_assembler_->handler(), fragment_limit);
    total += fee_sub_->poll(fee_assembler_->handler(), fragment_limit);
    total += funding_sub_->poll(funding_assembler_->handler(), fragment_limit);
    total += status_sub_->poll(status_assembler_->handler(), fragment_limit);
    return total;
}

}  // namespace bpt::strategy::refdata

#pragma once

/// @file
/// AeronRefdataClient<Handler> — Aeron-backed concrete refdata client.
/// Templated on the Handler that receives parsed events — `StrategyService`
/// in prod. Removes std::function indirection from snapshot / delta /
/// fee / funding / status fragment handlers.

#include "strategy/refdata/i_refdata_client.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/FeeSchedule.h>
#include <messages/FundingRate.h>
#include <messages/MessageHeader.h>
#include <messages/RefDataDelta.h>
#include <messages/RefDataError.h>
#include <messages/RefDataErrorType.h>
#include <messages/RefDataReady.h>
#include <messages/RefDataSnapshot.h>
#include <messages/RefDataSubscriptionRequest.h>

#include <bpt_common/aeron/publisher.h>
#include <bpt_common/aeron/subscriber.h>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <strategy/config/aeron_config.h>
#include <string>
#include <vector>

namespace bpt::strategy::refdata {

template <class Handler>
class AeronRefdataClient final : public IRefdataClient {
public:
    AeronRefdataClient(std::shared_ptr<aeron::Aeron> aeron,
                       const config::AeronConfig::Refdata& streams,
                       uint64_t max_staleness_ns)
        : fee_cache_(max_staleness_ns),
          funding_rate_cache_(max_staleness_ns) {
        ctrl_pub_ = std::make_unique<bpt::common::aeron::Publisher>(
            aeron,
            streams.control.channel,
            streams.control.stream_id,
            bpt::common::aeron::Publisher::Policy::kRetryOnBackpressure);
        snap_sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            aeron,
            streams.snapshot.channel,
            streams.snapshot.stream_id,
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_snapshot_fragment(buf, offset, length, hdr); });
        delta_sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            aeron,
            streams.delta.channel,
            streams.delta.stream_id,
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_delta_fragment(buf, offset, length, hdr); });
        fee_sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            aeron,
            streams.fee_schedule.channel,
            streams.fee_schedule.stream_id,
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_fee_schedule_fragment(buf, offset, length, hdr); });
        funding_sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            aeron,
            streams.funding_rate.channel,
            streams.funding_rate.stream_id,
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_funding_rate_fragment(buf, offset, length, hdr); });
        status_sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            aeron,
            streams.status.channel,
            streams.status.stream_id,
            [this](aeron::AtomicBuffer& buf,
                   aeron::util::index_t offset,
                   aeron::util::index_t length,
                   aeron::Header& hdr) { handle_status_fragment(buf, offset, length, hdr); });

        bpt::common::log::info("RefdataClient connected: ctrl={} snap={} delta={} fee={} funding={} status={}",
                               streams.control.stream_id,
                               streams.snapshot.stream_id,
                               streams.delta.stream_id,
                               streams.fee_schedule.stream_id,
                               streams.funding_rate.stream_id,
                               streams.status.stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    void subscribe(uint64_t correlation_id, std::vector<CanonicalFilter> filters = {}) override {
        using bpt::messages::MessageHeader;
        using bpt::messages::RefDataSubscriptionRequest;

        correlation_id_ = correlation_id;

        const uint16_t nf = static_cast<uint16_t>(filters.size());

        std::size_t buf_size = MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength() +
                               RefDataSubscriptionRequest::Instruments::sbeHeaderSize() +
                               RefDataSubscriptionRequest::CanonicalFilter::sbeHeaderSize() +
                               nf * RefDataSubscriptionRequest::CanonicalFilter::sbeBlockLength();

        std::vector<char> buf(buf_size, '\0');

        RefDataSubscriptionRequest req;
        req.wrapAndApplyHeader(buf.data(), 0, buf_size)
            .correlationId(correlation_id)
            .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

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
        ctrl_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(buf_size));

        bpt::common::log::info("Subscription request sent: correlation_id={} canonical_filters={}", correlation_id, nf);
    }

    int poll(int fragment_limit = 10) override {
        int total = 0;
        total += snap_sub_->poll(fragment_limit);
        total += delta_sub_->poll(fragment_limit);
        total += fee_sub_->poll(fragment_limit);
        total += funding_sub_->poll(fragment_limit);
        total += status_sub_->poll(fragment_limit);
        return total;
    }

    [[nodiscard]] uint64_t last_heartbeat_ns() const override { return last_heartbeat_ns_; }

    [[nodiscard]] const InstrumentCache& cache() const override { return cache_; }
    [[nodiscard]] const FeeCache& fee_cache() const override { return fee_cache_; }
    [[nodiscard]] const FundingRateCache& funding_rate_cache() const override { return funding_rate_cache_; }

private:
    void handle_snapshot_fragment(aeron::AtomicBuffer& buffer,
                                  aeron::util::index_t offset,
                                  aeron::util::index_t length,
                                  aeron::Header& /*header*/) {
        using bpt::messages::MessageHeader;
        using bpt::messages::RefDataSnapshot;

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

        if (handler_ != nullptr) [[likely]]
            handler_->on_refdata_snapshot_complete(cache_);
    }

    void handle_delta_fragment(aeron::AtomicBuffer& buffer,
                               aeron::util::index_t offset,
                               aeron::util::index_t length,
                               aeron::Header& /*header*/) {
        using bpt::messages::DeltaUpdateType;
        using bpt::messages::MessageHeader;
        using bpt::messages::RefDataDelta;

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

        if (update_type == DeltaUpdateType::NULL_VALUE) {
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
            if (handler_ != nullptr) [[likely]]
                handler_->on_refdata_gap_detected();
            return;
        }

        if (update_type == DeltaUpdateType::REMOVE) {
            fee_cache_.remove(msg.instrumentId());
            funding_rate_cache_.remove(msg.instrumentId());
        }

        if (handler_ != nullptr) [[likely]] {
            if (auto inst = cache_.get(msg.instrumentId()))
                handler_->on_refdata_delta(*inst, update_type);
        }
    }

    void handle_fee_schedule_fragment(aeron::AtomicBuffer& buffer,
                                      aeron::util::index_t offset,
                                      aeron::util::index_t length,
                                      aeron::Header& /*header*/) {
        using bpt::messages::ExchangeId;
        using bpt::messages::FeeSchedule;
        using bpt::messages::MessageHeader;

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

        bpt::common::log::debug("FeeSchedule: exchange={} instrument={} maker={}bps taker={}bps",
                                ExchangeId::c_str(msg.exchangeId()),
                                msg.instrumentId(),
                                msg.makerFeeBps(),
                                msg.takerFeeBps());
    }

    void handle_funding_rate_fragment(aeron::AtomicBuffer& buffer,
                                      aeron::util::index_t offset,
                                      aeron::util::index_t length,
                                      aeron::Header& /*header*/) {
        using bpt::messages::ExchangeId;
        using bpt::messages::FundingRate;
        using bpt::messages::MessageHeader;

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

        bpt::common::log::debug("FundingRate: exchange={} instrument={} rate={}bps",
                                ExchangeId::c_str(msg.exchangeId()),
                                msg.instrumentId(),
                                msg.rateBps());
    }

    void handle_status_fragment(aeron::AtomicBuffer& buffer,
                                aeron::util::index_t offset,
                                aeron::util::index_t length,
                                aeron::Header& /*header*/) {
        using bpt::messages::ExchangeId;
        using bpt::messages::MessageHeader;
        using bpt::messages::RefDataError;
        using bpt::messages::RefDataErrorType;
        using bpt::messages::RefDataReady;

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

            bpt::common::log::debug("RefDataReady: exchanges=0x{:02x} instruments={} fee_schedules={} funding_rates={}",
                                    msg.exchangesLoaded(),
                                    msg.instrumentCount(),
                                    msg.feeSchedulesLoaded(),
                                    msg.fundingRatesLoaded());

            if (handler_ != nullptr) [[likely]]
                handler_->on_refdata_ready(msg.exchangesLoaded(),
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

            bpt::common::log::error("RefDataError: type={} exchange={} instrument={}",
                                    RefDataErrorType::c_str(msg.errorType()),
                                    ExchangeId::c_str(msg.exchangeId()),
                                    msg.instrumentId());

            if (handler_ != nullptr) [[likely]]
                handler_->on_refdata_error(msg.errorType(), msg.exchangeId(), msg.instrumentId());
        }
    }

    std::unique_ptr<bpt::common::aeron::Publisher> ctrl_pub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> snap_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> delta_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> fee_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> funding_sub_;
    std::unique_ptr<bpt::common::aeron::Subscriber> status_sub_;

    FeeCache fee_cache_;
    FundingRateCache funding_rate_cache_;

    InstrumentCache cache_;
    Handler* handler_{nullptr};
    uint64_t correlation_id_{0};
    uint64_t last_heartbeat_ns_{0};
};

}  // namespace bpt::strategy::refdata

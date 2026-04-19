#include "pricer/refdata/refdata_subscriber.h"

#include <messages/DeltaUpdateType.h>
#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>
#include <messages/MessageHeader.h>
#include <messages/OptionSide.h>
#include <messages/RefDataDelta.h>
#include <messages/RefDataSnapshot.h>
#include <messages/RefDataSubscriptionRequest.h>

#include <x86intrin.h>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/util/tsc_clock.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <bpt_common/logging.h>

namespace bpt::pricer::refdata {

static bpt::messages::ExchangeId::Value exchange_from_string(const std::string& ex) {
    using EX = bpt::messages::ExchangeId;
    if (ex == "BINANCE")
        return EX::BINANCE;
    if (ex == "OKX")
        return EX::OKX;
    if (ex == "HYPERLIQUID")
        return EX::HYPERLIQUID;
    if (ex == "DERIBIT")
        return EX::DERIBIT;
    return EX::NULL_VALUE;
}

static std::string trim_null(const char* data, size_t len) {
    std::string s(data, len);
    auto pos = s.find('\0');
    if (pos != std::string::npos)
        s.resize(pos);
    return s;
}

RefdataSubscriber::RefdataSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                                     const std::string& snapshot_channel,
                                     int32_t snapshot_stream_id,
                                     const std::string& delta_channel,
                                     int32_t delta_stream_id,
                                     const std::string& control_channel,
                                     int32_t control_stream_id) {
    snapshot_sub_ = bpt::common::aeron::wait_for_subscription(aeron, snapshot_channel, snapshot_stream_id);
    delta_sub_ = bpt::common::aeron::wait_for_subscription(aeron, delta_channel, delta_stream_id);
    if (control_stream_id != 0)
        ctrl_pub_ = bpt::common::aeron::wait_for_publication(aeron, control_channel, control_stream_id);

    bpt::common::log::info("[RefdataSubscriber] Snapshot + delta subscriptions ready");
    snap_assembler_ = std::make_unique<aeron::FragmentAssembler>(
        [this](aeron::AtomicBuffer& buffer,
               aeron::util::index_t offset,
               aeron::util::index_t length,
               aeron::Header& header) { on_snapshot_fragment(buffer, offset, length, header); });

    if (ctrl_pub_)
        bpt::common::log::info("[RefdataSubscriber] Control publication ready");
}

void RefdataSubscriber::send_subscription_request(uint64_t correlation_id) {
    if (!ctrl_pub_) {
        bpt::common::log::warn("[RefdataSubscriber] send_subscription_request: no control publication");
        return;
    }

    using namespace bpt::messages;

    // Empty filters — request all instruments, like fenrir does.
    std::size_t buf_size = MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength() +
                           RefDataSubscriptionRequest::Instruments::sbeHeaderSize() +
                           RefDataSubscriptionRequest::CanonicalFilter::sbeHeaderSize();

    std::vector<char> buf(buf_size, '\0');

    RefDataSubscriptionRequest req;
    req.wrapAndApplyHeader(buf.data(), 0, buf_size)
        .correlationId(correlation_id)
        .timestampNs(bpt::common::util::TscClock::now_epoch_ns());

    req.instrumentsCount(0);
    req.canonicalFilterCount(0);

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf.data()), static_cast<aeron::util::index_t>(buf_size));
    // Retry for up to 10s — Aeron offer() returns NOT_CONNECTED until the
    // subscriber (bpt-refdata) has registered the image.  Sleep between retries
    // so we don't spin the CPU waiting for the driver.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    long result = 0;
    while ((result = ctrl_pub_->offer(ab, 0, static_cast<aeron::util::index_t>(buf_size))) < 0) {
        if (std::chrono::steady_clock::now() > deadline) {
            bpt::common::log::error("[RefdataSubscriber] subscription request offer timed out (last result={})", result);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    bpt::common::log::info("[RefdataSubscriber] Subscription request sent: correlation_id={}", correlation_id);
}

int RefdataSubscriber::poll(int fragment_limit) {
    int total = 0;
    if (snapshot_sub_ && snap_assembler_) {
        total += snapshot_sub_->poll(snap_assembler_->handler(), fragment_limit);
    }
    if (delta_sub_) {
        total +=
            delta_sub_->poll([this](const aeron::concurrent::AtomicBuffer& buffer,
                                    aeron::util::index_t offset,
                                    aeron::util::index_t length,
                                    const aeron::Header& header) { on_delta_fragment(buffer, offset, length, header); },
                             fragment_limit);
    }
    return total;
}

void RefdataSubscriber::on_snapshot_fragment(const aeron::concurrent::AtomicBuffer& buffer,
                                             aeron::util::index_t offset,
                                             aeron::util::index_t length,
                                             const aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
        return;

    MessageHeader hdr;
    hdr.wrap(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
             0,
             MessageHeader::sbeSchemaVersion(),
             static_cast<uint64_t>(length));

    if (hdr.templateId() != RefDataSnapshot::sbeTemplateId())
        return;

    RefDataSnapshot snap;
    snap.wrapForDecode(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
                       MessageHeader::encodedLength(),
                       hdr.blockLength(),
                       hdr.version(),
                       static_cast<uint64_t>(length));

    auto& instruments = snap.instruments();
    int total_count = 0, option_count = 0;
    while (instruments.hasNext()) {
        instruments.next();
        ++total_count;

        if (instruments.instrumentType() == InstrumentType::PERPETUAL) {
            auto exchange_str = trim_null(instruments.exchange(), 8);
            auto underlying_str = trim_null(instruments.underlying(), 24);
            refdata::PerpInstrument pi{
                .instrument_id = instruments.instrumentId(),
                .underlying = underlying_str,
                .exchange = exchange_str,
                .exchange_id = exchange_from_string(exchange_str),
            };
            bpt::common::log::info("[RefdataSubscriber] Perp instrument: {} id={} exchange={}",
                           underlying_str,
                           instruments.instrumentId(),
                           exchange_str);
            if (on_perp_)
                on_perp_(pi);
            continue;
        }

        if (instruments.instrumentType() != InstrumentType::OPTION) {
            bpt::common::log::info("[RefdataSubscriber] Skipping instrument type={} id={}",
                           static_cast<int>(instruments.instrumentType()),
                           instruments.instrumentId());
            continue;
        }
        ++option_count;

        auto exchange_str = trim_null(instruments.exchange(), 8);
        auto underlying_str = trim_null(instruments.underlying(), 24);

        surface::OptionInstrument oi{
            .instrument_id = instruments.instrumentId(),
            .underlying = underlying_str,
            .exchange = exchange_str,
            .exchange_id = exchange_from_string(exchange_str),
            .expiry_date = instruments.expiryDate(),
            .strike_price = instruments.strikePrice(),
            .is_call = (instruments.optionSide() == OptionSide::CALL),
        };

        if (on_option_)
            on_option_(oi);
    }
    bpt::common::log::info("[RefdataSubscriber] Snapshot: {} total instruments, {} options", total_count, option_count);
}

void RefdataSubscriber::on_delta_fragment(const aeron::concurrent::AtomicBuffer& buffer,
                                          aeron::util::index_t offset,
                                          aeron::util::index_t length,
                                          const aeron::Header& /*header*/) {
    using namespace bpt::messages;

    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength()))
        return;

    MessageHeader hdr;
    hdr.wrap(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
             0,
             MessageHeader::sbeSchemaVersion(),
             static_cast<uint64_t>(length));

    if (hdr.templateId() != RefDataDelta::sbeTemplateId())
        return;

    RefDataDelta delta;
    delta.wrapForDecode(const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset)),
                        MessageHeader::encodedLength(),
                        hdr.blockLength(),
                        hdr.version(),
                        static_cast<uint64_t>(length));

    if (delta.updateType() == DeltaUpdateType::REMOVE) {
        if (on_remove_)
            on_remove_(delta.instrumentId());
        return;
    }

    // ADD or MODIFY — only care about options
    if (delta.instrumentType() != InstrumentType::OPTION)
        return;

    auto exchange_str = trim_null(delta.exchange(), 8);
    auto underlying_str = trim_null(delta.underlying(), 24);

    surface::OptionInstrument oi{
        .instrument_id = delta.instrumentId(),
        .underlying = underlying_str,
        .exchange = exchange_str,
        .exchange_id = exchange_from_string(exchange_str),
        .expiry_date = delta.expiryDate(),
        .strike_price = delta.strikePrice(),
        .is_call = (delta.optionSide() == OptionSide::CALL),
    };

    if (on_option_)
        on_option_(oi);
}

}  // namespace bpt::pricer::refdata

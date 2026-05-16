#include "refdata/messaging/subscribers/refdata_control_subscriber.h"

#include <messages/MessageHeader.h>
#include <messages/RefDataSubscriptionRequest.h>

#include <bpt_common/logging.h>
#include <cstring>

namespace bpt::refdata::messaging {

RefdataControlSubscriber::RefdataControlSubscriber(std::shared_ptr<::aeron::Aeron> aeron,
                                                   const std::string& channel,
                                                   int stream_id) {
    subscription_ = std::make_unique<bpt::common::aeron::Subscriber>(
        std::move(aeron),
        channel,
        stream_id,
        [this](::aeron::AtomicBuffer& ab,
               ::aeron::util::index_t offset,
               ::aeron::util::index_t length,
               ::aeron::Header&) {
            using namespace bpt::messages;

            if (static_cast<std::size_t>(length) <
                MessageHeader::encodedLength() + RefDataSubscriptionRequest::sbeBlockLength()) {
                bpt::common::log::warn("Control: short fragment ({} bytes), ignoring", length);
                return;
            }

            char* data = reinterpret_cast<char*>(ab.buffer() + offset);
            std::uint64_t buf_len = static_cast<std::uint64_t>(length);

            MessageHeader hdr(data, buf_len);
            if (hdr.templateId() != RefDataSubscriptionRequest::sbeTemplateId()) {
                bpt::common::log::warn("Control: unexpected templateId {}, ignoring", hdr.templateId());
                return;
            }

            RefDataSubscriptionRequest sbe_req;
            sbe_req.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), buf_len);

            RefdataRequest req;
            req.correlation_id = sbe_req.correlationId();

            auto& instr_group = sbe_req.instruments();
            while (instr_group.hasNext()) {
                instr_group.next();
                InstrumentFilter f{};
                std::memcpy(f.symbol, instr_group.symbol(), sizeof(f.symbol));
                std::memcpy(f.exchange, instr_group.exchange(), sizeof(f.exchange));
                req.instruments.push_back(f);
            }

            auto& cf_group = sbe_req.canonicalFilter();
            while (cf_group.hasNext()) {
                cf_group.next();
                CanonicalFilter cf{};
                std::memcpy(cf.base_currency, cf_group.baseCurrency(), sizeof(cf.base_currency));
                std::memcpy(cf.quote_currency, cf_group.quoteCurrency(), sizeof(cf.quote_currency));
                cf.instrument_type = cf_group.instrumentType();
                std::memcpy(cf.exchange, cf_group.exchange(), sizeof(cf.exchange));
                req.canonical_filters.push_back(cf);
            }

            if (current_handler_)
                current_handler_(req);
        });
}

int RefdataControlSubscriber::poll(RequestHandler handler) {
    if (!subscription_->is_connected())
        return 0;

    current_handler_ = handler;
    int fragments = subscription_->poll(10);
    current_handler_ = nullptr;
    return fragments;
}

}  // namespace bpt::refdata::messaging

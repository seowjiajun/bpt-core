#pragma once

/// \file
/// Aeron concrete: refdata-snapshot consumer scoped to perpetual
/// instruments only. CRTP-templated. Reassembles multi-fragment
/// snapshots with FragmentAssembler.

#include "radar/messaging/subscribers/api/refdata_perp_subscriber.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/ExchangeId.h>
#include <messages/InstrumentType.h>
#include <messages/MessageHeader.h>
#include <messages/RefDataSnapshot.h>

#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace bpt::radar::messaging::aeron {

template <class Handler>
class RefdataPerpSubscriber final : public api::RefdataPerpSubscriber {
public:
    RefdataPerpSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int stream_id) {
        sub_ = bpt::common::aeron::wait_for_subscription(aeron, channel, stream_id);
        assembler_ = std::make_unique<::aeron::FragmentAssembler>(
            [this](::aeron::AtomicBuffer& buffer,
                   ::aeron::util::index_t offset,
                   ::aeron::util::index_t length,
                   ::aeron::Header& header) { handle_fragment(buffer, offset, length, header); });
        bpt::common::log::info("[RefdataPerpSubscriber] ready on stream {}", stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 4) override {
        if (!sub_)
            return 0;
        return sub_->poll(assembler_->handler(), fragment_limit);
    }

private:
    static std::string trim_null(const char* p, std::size_t n) {
        auto len = ::strnlen(p, n);
        return std::string(p, len);
    }

    void handle_fragment(::aeron::AtomicBuffer& buffer,
                         ::aeron::util::index_t offset,
                         ::aeron::util::index_t length,
                         ::aeron::Header& /*header*/) {
        using bpt::messages::ExchangeId;
        using bpt::messages::InstrumentType;
        using bpt::messages::MessageHeader;
        using bpt::messages::RefDataSnapshot;

        if (length < static_cast<::aeron::util::index_t>(MessageHeader::encodedLength()))
            return;

        char* data = reinterpret_cast<char*>(buffer.buffer()) + offset;
        MessageHeader hdr;
        hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

        if (hdr.templateId() != RefDataSnapshot::sbeTemplateId())
            return;

        RefDataSnapshot snap;
        snap.wrapForDecode(data,
                           MessageHeader::encodedLength(),
                           hdr.blockLength(),
                           hdr.version(),
                           static_cast<uint64_t>(length));

        if (handler_ == nullptr) [[unlikely]]
            return;

        auto& instruments = snap.instruments();
        while (instruments.hasNext()) {
            instruments.next();
            if (instruments.instrumentType() != InstrumentType::PERPETUAL)
                continue;

            PerpInfo info;
            info.instrument_id = instruments.instrumentId();
            info.underlying = trim_null(instruments.underlying(), 24);

            auto venue = trim_null(instruments.exchange(), 8);
            if (venue == "BINANCE")
                info.exchange_id = static_cast<uint8_t>(ExchangeId::BINANCE);
            else if (venue == "OKX")
                info.exchange_id = static_cast<uint8_t>(ExchangeId::OKX);
            else if (venue == "HYPERLIQ" || venue == "HYPERLIQUID")
                info.exchange_id = static_cast<uint8_t>(ExchangeId::HYPERLIQUID);
            else if (venue == "DERIBIT")
                info.exchange_id = static_cast<uint8_t>(ExchangeId::DERIBIT);
            else
                continue;

            handler_->on_refdata_perp(info);
        }
    }

    std::shared_ptr<::aeron::Subscription> sub_;
    std::unique_ptr<::aeron::FragmentAssembler> assembler_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::radar::messaging::aeron

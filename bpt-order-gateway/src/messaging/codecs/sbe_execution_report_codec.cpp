#include "order_gateway/messaging/codecs/sbe_execution_report_codec.h"

#include <messages/ExecutionReport.h>
#include <messages/MessageHeader.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bpt::order_gateway::messaging {

using bpt::messages::ExecutionReport;
using bpt::messages::MessageHeader;

std::span<const std::byte> SbeExecutionReportCodec::encode(const api::ExecReport& m, std::span<std::byte> scratch) {
    auto* buf = reinterpret_cast<char*>(scratch.data());
    std::memset(buf, 0, scratch.size());

    // SBE Char8 slot is fixed-width; zero-pad short ccy strings so the
    // unused trailing bytes are deterministic.
    char ccy_pad[8] = {0};
    const std::size_t ccy_len = std::min(m.fee_currency.size(), sizeof(ccy_pad));
    std::memcpy(ccy_pad, m.fee_currency.data(), ccy_len);

    ExecutionReport msg;
    msg.wrapAndApplyHeader(buf, 0, scratch.size())
        .orderId(m.order_id)
        .exchangeOrderId(m.exchange_order_id)
        .exchangeId(m.exchange_id)
        .instrumentId(m.instrument_id)
        .status(m.status)
        .side(m.side)
        .orderType(m.order_type)
        .price(m.price)
        .filledQty(m.filled_qty)
        .remainingQty(m.remaining_qty)
        .rejectReason(m.reject_reason)
        .fee(m.fee)
        .putFeeCurrency(ccy_pad)
        .timestampNs(m.exchange_ts_ns)
        .localTsNs(m.local_ts_ns);

    const auto total = MessageHeader::encodedLength() + ExecutionReport::sbeBlockLength();
    return scratch.subspan(0, total);
}

api::ExecReport SbeExecutionReportCodec::decode(std::span<const std::byte> bytes) {
    if (bytes.size() < MessageHeader::encodedLength())
        throw std::runtime_error("SbeExecutionReportCodec::decode: too short");

    auto* data = const_cast<char*>(reinterpret_cast<const char*>(bytes.data()));
    const uint64_t len = bytes.size();

    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), len);

    if (hdr.templateId() != ExecutionReport::sbeTemplateId())
        throw std::runtime_error("SbeExecutionReportCodec::decode: wrong template id");

    ExecutionReport msg;
    msg.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(), hdr.version(), len);

    return api::ExecReport{
        .order_id = msg.orderId(),
        .exchange_order_id = msg.exchangeOrderId(),
        .exchange_id = msg.exchangeId(),
        .instrument_id = msg.instrumentId(),
        .status = msg.status(),
        .side = msg.side(),
        .order_type = msg.orderType(),
        .price = msg.price(),
        .filled_qty = msg.filledQty(),
        .remaining_qty = msg.remainingQty(),
        .reject_reason = msg.rejectReason(),
        .fee = msg.fee(),
        .fee_currency = msg.getFeeCurrencyAsString(),
        .exchange_ts_ns = msg.timestampNs(),
        .local_ts_ns = msg.localTsNs(),
    };
}

}  // namespace bpt::order_gateway::messaging

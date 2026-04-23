#include "order_gateway/messaging/exec_report_publisher.h"

#include <messages/ExecutionReport.h>
#include <messages/MessageHeader.h>

namespace bpt::order_gateway::messaging {

using Policy = bpt::common::aeron::Publisher::Policy;

ExecReportPublisher::ExecReportPublisher(std::shared_ptr<::aeron::Aeron> aeron,
                                         const std::string& channel,
                                         int stream_id)
    // Exec reports must reach the strategy — they drive position
    // tracking. Spin through back-pressure; drop if no subscriber
    // rather than hang (strategy down → gateway isn't useful anyway).
    : publisher_(std::move(aeron), channel, stream_id, Policy::kRetryOnBackpressure) {}

void ExecReportPublisher::publish(uint64_t order_id,
                                  uint64_t exchange_order_id,
                                  bpt::messages::ExchangeId::Value exchange_id,
                                  uint64_t instrument_id,
                                  bpt::messages::ExecStatus::Value status,
                                  bpt::messages::OrderSide::Value side,
                                  bpt::messages::OrderType::Value order_type,
                                  int64_t price,
                                  uint64_t filled_qty,
                                  uint64_t remaining_qty,
                                  bpt::messages::RejectReason::Value reject_reason,
                                  int64_t fee,
                                  bpt::messages::FeeCurrency::Value fee_currency,
                                  uint64_t exchange_ts_ns,
                                  uint64_t local_ts_ns) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + ExecutionReport::sbeBlockLength();
    char buf[kBufSize]{};

    ExecutionReport msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .orderId(order_id)
        .exchangeOrderId(exchange_order_id)
        .exchangeId(exchange_id)
        .instrumentId(instrument_id)
        .status(status)
        .side(side)
        .orderType(order_type)
        .price(price)
        .filledQty(filled_qty)
        .remainingQty(remaining_qty)
        .rejectReason(reject_reason)
        .fee(fee)
        .feeCurrency(fee_currency)
        .timestampNs(exchange_ts_ns)
        .localTsNs(local_ts_ns);

    ::aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<::aeron::util::index_t>(kBufSize));
    publisher_.offer(ab, 0, static_cast<::aeron::util::index_t>(kBufSize));
}

}  // namespace bpt::order_gateway::messaging

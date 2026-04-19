#include "order_gateway/messaging/exec_report_publisher.h"

#include <messages/ExecutionReport.h>
#include <messages/MessageHeader.h>

#include <thread>
#include <bpt_common/aeron/aeron_utils.h>

namespace bpt::order_gateway::messaging {

ExecReportPublisher::ExecReportPublisher(std::shared_ptr<aeron::Aeron> aeron,
                                         const std::string& channel,
                                         int stream_id) {
    publication_ = bpt::common::aeron::wait_for_publication(aeron, channel, stream_id);
}

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

    aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf), static_cast<aeron::util::index_t>(kBufSize));

    while (publication_->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize)) < 0) {
        std::this_thread::yield();
    }
}

}  // namespace bpt::order_gateway::messaging

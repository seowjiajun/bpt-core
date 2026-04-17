#pragma once

#include <Aeron.h>

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <memory>
#include <string>

namespace bpt::order_gateway::messaging {

class ExecReportPublisher {
public:
    ExecReportPublisher(std::shared_ptr<aeron::Aeron> aeron, const std::string& channel, int stream_id);

    void publish(uint64_t order_id,
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
                 uint64_t local_ts_ns);

private:
    std::shared_ptr<aeron::Publication> publication_;
};

}  // namespace bpt::order_gateway::messaging

#pragma once

#include "strategy/order/i_order_gateway_client.h"

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <messages/AccountSnapshotRequest.h>
#include <messages/CancelAll.h>
#include <messages/CancelOrder.h>
#include <messages/ModifyOrder.h>
#include <messages/NewOrder.h>

#include <memory>
#include <string>
#include <bpt_common/aeron/publisher.h>

namespace bpt::strategy::order {

// Aeron-backed IOrderGatewayClient — publishes order actions to
// bpt-order-gateway and polls the exec-report / heartbeat /
// account-snapshot streams it publishes back. Production path.
class AeronOrderGatewayClient : public IOrderGatewayClient {
public:
    AeronOrderGatewayClient(std::shared_ptr<aeron::Aeron> aeron,
                            const std::string& channel,
                            int order_stream,
                            int exec_report_stream,
                            int heartbeat_stream,
                            int account_snapshot_stream);

    [[nodiscard]] bool send_new_order(uint64_t order_id,
                                      bpt::messages::ExchangeId::Value exchange_id,
                                      uint64_t instrument_id,
                                      bpt::messages::OrderSide::Value side,
                                      bpt::messages::OrderType::Value order_type,
                                      bpt::messages::TimeInForce::Value tif,
                                      int64_t price,
                                      uint64_t quantity,
                                      const std::string& exchange_symbol) override;

    void send_cancel(uint64_t order_id,
                     bpt::messages::ExchangeId::Value exchange_id,
                     uint64_t instrument_id) override;

    void send_cancel_all(bpt::messages::ExchangeId::Value exchange_id,
                         uint64_t instrument_id) override;

    void send_modify(uint64_t order_id,
                     bpt::messages::ExchangeId::Value exchange_id,
                     uint64_t instrument_id,
                     int64_t new_price,
                     uint64_t new_quantity) override;

    void send_account_snapshot_request(bpt::messages::ExchangeId::Value exchange_id,
                                        uint64_t correlation_id) override;

    int poll(int fragment_limit = 10) override;

    [[nodiscard]] uint64_t last_heartbeat_ns() const override { return last_heartbeat_ns_; }

private:
    void handle_exec_report_fragment(aeron::AtomicBuffer& buf,
                                     aeron::util::index_t offset,
                                     aeron::util::index_t length,
                                     aeron::Header& hdr);
    void handle_heartbeat_fragment(aeron::AtomicBuffer& buf,
                                   aeron::util::index_t offset,
                                   aeron::util::index_t length,
                                   aeron::Header& hdr);
    void handle_account_snapshot_fragment(aeron::AtomicBuffer& buf,
                                          aeron::util::index_t offset,
                                          aeron::util::index_t length,
                                          aeron::Header& hdr);

    std::unique_ptr<bpt::common::aeron::Publisher> order_pub_;
    std::shared_ptr<aeron::Subscription> exec_report_sub_;
    std::shared_ptr<aeron::Subscription> heartbeat_sub_;
    std::shared_ptr<aeron::Subscription> account_snapshot_sub_;
    std::unique_ptr<aeron::FragmentAssembler> exec_assembler_;
    std::unique_ptr<aeron::FragmentAssembler> hb_assembler_;
    std::unique_ptr<aeron::FragmentAssembler> account_snapshot_assembler_;
    uint64_t last_heartbeat_ns_{0};
};

}  // namespace bpt::strategy::order

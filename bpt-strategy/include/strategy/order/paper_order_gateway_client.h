#pragma once

// PaperOrderGatewayClient — IOrderGatewayClient impl used in canary /
// shadow runs. Receives order submissions, delegates to PaperFillEngine,
// and synthesises SBE ExecutionReports on poll() so the strategy's
// normal on_exec_report path fires exactly as it does in live trading.
//
// Market data flow: StrategyApp explicitly feeds on_bbo / on_trade to
// this client BEFORE dispatching to the strategy, so any fills triggered
// by the tick arrive on the strategy's next poll — mirroring the real-
// world ordering where an exchange's match-then-publish sequence is
// observed downstream.
//
// Optional dashboard visibility: if constructed with an Aeron exec-report
// publication, every dispatched event is ALSO offered to stream 3002 so
// bpt-bridge sees paper fills identically to live fills (blotter, chart
// overlay, etc.). Without the publication, dispatch is in-process only
// (the pattern used by unit tests).

#include "strategy/order/i_order_gateway_client.h"
#include "strategy/order/paper_fill_engine.h"

#include <Aeron.h>

#include <memory>
#include <string>

namespace bpt::strategy::order {

class PaperOrderGatewayClient : public IOrderGatewayClient {
public:
    PaperOrderGatewayClient();

    // Aeron-publishing variant. Strategy uses this in paper mode so that
    // bridge / dashboard subscribers on exec_report_stream see paper
    // fills — without it the in-process callback is the only consumer
    // and the blotter stays empty.
    PaperOrderGatewayClient(std::shared_ptr<aeron::Aeron> aeron,
                            const std::string& exec_report_channel,
                            int exec_report_stream);

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

    // Paper mode has no real gateway — returning "now" keeps
    // StrategyApp's liveness gate from tripping on a healthy run.
    [[nodiscard]] uint64_t last_heartbeat_ns() const override;

    // Feed market data into the fill engine. StrategyApp calls these
    // from its MdClient callback wiring when running in paper mode.
    // Keyed by instrument_id only — SBE MD messages don't carry an
    // exchange field and our instrument ids are globally unique.
    void feed_bbo(uint64_t instrument_id, double bid, double ask, uint64_t ts_ns);
    void feed_trade(uint64_t instrument_id, double price, double qty, uint64_t ts_ns);

    // Accessor for tests / metrics.
    PaperFillEngine& engine() { return engine_; }

private:
    PaperFillEngine engine_;
    // Optional publisher to stream 3002 so bridge / dashboard see paper
    // fills. Null for in-process-only constructions (unit tests).
    std::shared_ptr<aeron::Publication> exec_report_pub_;
};

}  // namespace bpt::strategy::order

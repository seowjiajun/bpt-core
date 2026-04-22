#include "strategy/order/paper_order_gateway_client.h"

#include <messages/ExecutionReport.h>
#include <messages/FeeCurrency.h>
#include <messages/MessageHeader.h>

#include <chrono>
#include <cstring>
#include <thread>
#include <bpt_common/aeron/aeron_utils.h>
#include <bpt_common/logging.h>

namespace bpt::strategy::order {

namespace {

uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Encode a PaperFillEvent into a stack buffer, optionally publish it to
// Aeron (so bridge / dashboard see paper fills), then decode it in place
// and fire the in-process callback. The buffer outlives the callback, so
// the view it exposes is valid for the duration of the dispatch.
void dispatch_event(const PaperFillEvent& ev,
                    const IOrderGatewayClient::OnExecReportFn& cb,
                    aeron::Publication* exec_report_pub) {
    using namespace bpt::messages;

    constexpr std::size_t kBufSize =
        MessageHeader::encodedLength() + ExecutionReport::sbeBlockLength();
    char buf[kBufSize]{};
    ExecutionReport msg;
    msg.wrapAndApplyHeader(buf, 0, kBufSize)
        .orderId(ev.order_id)
        .exchangeOrderId(ev.order_id)  // paper has no separate exch id; mirror client id
        .exchangeId(ev.exchange_id)
        .instrumentId(ev.instrument_id)
        .status(ev.status)
        .side(ev.side)
        .orderType(ev.order_type)
        .price(ev.price_e8)
        .filledQty(ev.filled_qty_e8)
        .remainingQty(ev.remaining_qty_e8)
        .rejectReason(ev.reject_reason)
        .fee(0)
        .feeCurrency(FeeCurrency::NULL_VALUE)
        .timestampNs(ev.ts_ns)
        .localTsNs(ev.ts_ns);

    // Publish FIRST so downstream subscribers (bridge) see the event at
    // the same wall-clock moment the in-process strategy callback does;
    // callback-first would delay the dashboard update behind any handler
    // work the strategy does.
    if (exec_report_pub) {
        aeron::AtomicBuffer ab(reinterpret_cast<uint8_t*>(buf),
                               static_cast<aeron::util::index_t>(kBufSize));
        while (exec_report_pub->offer(ab, 0, static_cast<aeron::util::index_t>(kBufSize)) < 0) {
            std::this_thread::yield();
        }
    }

    if (!cb)
        return;
    // Re-wrap as a decoder for the callback — same storage.
    ExecutionReport view;
    view.wrapForDecode(buf,
                       MessageHeader::encodedLength(),
                       ExecutionReport::sbeBlockLength(),
                       ExecutionReport::sbeSchemaVersion(),
                       kBufSize);
    cb(view);
}

}  // namespace

PaperOrderGatewayClient::PaperOrderGatewayClient() = default;

PaperOrderGatewayClient::PaperOrderGatewayClient(std::shared_ptr<aeron::Aeron> aeron,
                                                 const std::string& exec_report_channel,
                                                 int exec_report_stream) {
    exec_report_pub_ = bpt::common::aeron::wait_for_publication(
        aeron, exec_report_channel, exec_report_stream);
    bpt::common::log::info(
        "PaperOrderGatewayClient: exec-report publisher ready on {} stream {}",
        exec_report_channel, exec_report_stream);
}

bool PaperOrderGatewayClient::send_new_order(uint64_t order_id,
                                             bpt::messages::ExchangeId::Value exchange_id,
                                             uint64_t instrument_id,
                                             bpt::messages::OrderSide::Value side,
                                             bpt::messages::OrderType::Value order_type,
                                             bpt::messages::TimeInForce::Value tif,
                                             int64_t price,
                                             uint64_t quantity,
                                             const std::string& exchange_symbol) {
    // Mirror AeronOrderGatewayClient's pre-flight checks so paper and
    // live reject symmetrically — important so a canary catches the same
    // bad-order rejects the live path would.
    using namespace bpt::messages;
    if (quantity == 0) {
        bpt::common::log::warn("[PaperGW] Rejected order_id={}: quantity is zero", order_id);
        return false;
    }
    if (order_type != OrderType::MARKET && price <= 0) {
        bpt::common::log::warn("[PaperGW] Rejected order_id={}: price={} invalid for non-MARKET",
                               order_id, price);
        return false;
    }
    if (exchange_symbol.empty()) {
        bpt::common::log::warn("[PaperGW] Rejected order_id={}: empty exchange_symbol", order_id);
        return false;
    }

    PaperFillEngine::Order o;
    o.order_id = order_id;
    o.exchange_id = exchange_id;
    o.instrument_id = instrument_id;
    o.side = side;
    o.order_type = order_type;
    o.tif = tif;
    o.price_e8 = price;
    o.quantity_e8 = quantity;
    engine_.submit(o, now_ns());
    return true;
}

void PaperOrderGatewayClient::send_cancel(uint64_t order_id,
                                          bpt::messages::ExchangeId::Value exchange_id,
                                          uint64_t instrument_id) {
    engine_.cancel(order_id, exchange_id, instrument_id, now_ns());
}

void PaperOrderGatewayClient::send_cancel_all(bpt::messages::ExchangeId::Value exchange_id,
                                              uint64_t instrument_id) {
    engine_.cancel_all(exchange_id, instrument_id, now_ns());
}

void PaperOrderGatewayClient::send_modify(uint64_t order_id,
                                          bpt::messages::ExchangeId::Value exchange_id,
                                          uint64_t instrument_id,
                                          int64_t /*new_price*/,
                                          uint64_t /*new_quantity*/) {
    // MVP: model modify as a cancel — the strategy typically falls
    // back to placing a fresh order when a modify fails, and this
    // keeps the paper book consistent without the complexity of
    // preserving queue priority. Fire a CANCELLED event so the
    // strategy's cancel-pending tracking clears cleanly.
    engine_.cancel(order_id, exchange_id, instrument_id, now_ns());
}

void PaperOrderGatewayClient::send_account_snapshot_request(
    bpt::messages::ExchangeId::Value /*exchange_id*/,
    uint64_t /*correlation_id*/) {
    // Paper mode doesn't simulate balances — strategies that rely on
    // AccountSnapshot for startup gating need to handle the absence of
    // one (the startup gate will time out and proceed). Intentional
    // no-op so a canary run doesn't spam warnings.
}

int PaperOrderGatewayClient::poll(int fragment_limit) {
    return engine_.drain(fragment_limit, [this](const PaperFillEvent& ev) {
        dispatch_event(ev, on_exec_report, exec_report_pub_.get());
    });
}

uint64_t PaperOrderGatewayClient::last_heartbeat_ns() const {
    return now_ns();
}

void PaperOrderGatewayClient::feed_bbo(uint64_t instrument_id,
                                        double bid,
                                        double ask,
                                        uint64_t ts_ns) {
    engine_.on_bbo(instrument_id, bid, ask, ts_ns);
}

void PaperOrderGatewayClient::feed_trade(uint64_t instrument_id,
                                          double price,
                                          double qty,
                                          uint64_t ts_ns) {
    engine_.on_trade(instrument_id, price, qty, ts_ns);
}

}  // namespace bpt::strategy::order

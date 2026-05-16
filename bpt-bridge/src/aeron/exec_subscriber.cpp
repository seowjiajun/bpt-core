#include "bridge/aeron/exec_subscriber.h"

#include "bridge/aeron/sbe_decode.h"

#include <messages/ExecStatus.h>
#include <messages/ExecutionReport.h>
#include <messages/OrderSide.h>

#include <bpt_common/logging.h>

namespace bpt::bridge {

using bpt::messages::ExecStatus;
using bpt::messages::ExecutionReport;
using bpt::messages::OrderSide;

namespace {
constexpr double kPriceScale = 1e8;
constexpr double kQtyScale = 1e8;  // filled_qty = natural * 1e8
constexpr double kFeeScale = 1e8;
}  // namespace

ExecSubscriber::ExecSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id) {
    sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
        std::move(aeron),
        channel,
        stream_id,
        [this](::aeron::AtomicBuffer& b, ::aeron::util::index_t o, ::aeron::util::index_t l, ::aeron::Header& h) {
            on_fragment(b, o, l, h);
        });
    bpt::common::log::info("[bridge/Exec] subscribed on {} stream {}", channel, stream_id);
}

int ExecSubscriber::poll(int fragment_limit) {
    return sub_ ? sub_->poll(fragment_limit) : 0;
}

void ExecSubscriber::on_fragment(::aeron::AtomicBuffer& buffer,
                                 ::aeron::util::index_t offset,
                                 ::aeron::util::index_t length,
                                 ::aeron::Header& /*header*/) {
    decode_sbe_fragment<bpt::messages::ExecutionReport>(
        buffer,
        offset,
        length,
        [this](bpt::messages::ExecutionReport& msg) {
            const auto status = msg.status();
            const auto side = (msg.side() == OrderSide::BUY) ? encode::Side::Buy : encode::Side::Sell;
            const double price = static_cast<double>(msg.price()) / kPriceScale;

            // Fire order lifecycle event for all statuses so the dashboard can
            // track open/working orders.
            if (order_handler_) {
                OrderEvent ev{};
                ev.ts_ns = msg.timestampNs();
                ev.order_id = msg.orderId();
                ev.instrument_id = msg.instrumentId();
                ev.side = side;
                ev.status = static_cast<uint8_t>(status);
                ev.order_type = msg.orderTypeRaw();
                ev.price = price;
                ev.qty = static_cast<double>(msg.filledQty() + msg.remainingQty()) / kQtyScale;
                ev.filled_qty = static_cast<double>(msg.filledQty()) / kQtyScale;
                ev.remaining_qty = static_cast<double>(msg.remainingQty()) / kQtyScale;
                order_handler_(ev);
            }

            // Only forward real fills to the position tracker / equity curve.
            // OKX (and other venues) can emit PARTIAL exec reports with filled_qty=0
            // as order-state updates ("order is resting in book"). Those are not
            // actual executions and must not be counted as fills.
            if (status != ExecStatus::FILLED && status != ExecStatus::PARTIAL)
                return;
            if (msg.filledQty() == 0)
                return;

            Fill f{};
            f.ts_ns = msg.timestampNs();
            f.order_id = msg.orderId();
            f.instrument_id = msg.instrumentId();
            f.side = side;
            f.order_type = msg.orderTypeRaw();
            f.qty = static_cast<double>(msg.filledQty()) / kQtyScale;
            f.price = price;
            f.fee = static_cast<double>(msg.fee()) / kFeeScale;

            if (handler_)
                handler_(f);
        });
}

}  // namespace bpt::bridge

#include "bridge/exec_subscriber.h"

#include <bifrost_protocol/ExecStatus.h>
#include <bifrost_protocol/ExecutionReport.h>
#include <bifrost_protocol/MessageHeader.h>
#include <bifrost_protocol/OrderSide.h>

#include <chrono>
#include <thread>
#include <yggdrasil/logging.h>

namespace bridge {

namespace {
constexpr double kPriceScale = 1e8;
constexpr double kQtyScale   = 1e8;  // filled_qty = natural * 1e8
constexpr double kFeeScale   = 1e8;
}

ExecSubscriber::ExecSubscriber(std::shared_ptr<aeron::Aeron> aeron,
                               const std::string& channel,
                               int32_t stream_id) {
    const int64_t reg_id = aeron->addSubscription(channel, stream_id);
    for (int i = 0; i < 500; ++i) {
        sub_ = aeron->findSubscription(reg_id);
        if (sub_) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!sub_) {
        ygg::log::error("[bridge/Exec] failed to register subscription on {} stream {}", channel, stream_id);
    } else {
        ygg::log::info("[bridge/Exec] subscribed on {} stream {}", channel, stream_id);
    }
}

int ExecSubscriber::poll(int fragment_limit) {
    if (!sub_) return 0;
    return sub_->poll(
        [this](const aeron::concurrent::AtomicBuffer& b,
               aeron::util::index_t o,
               aeron::util::index_t l,
               const aeron::Header& h) { on_fragment(b, o, l, h); },
        fragment_limit);
}

void ExecSubscriber::on_fragment(const aeron::concurrent::AtomicBuffer& buffer,
                                 aeron::util::index_t offset,
                                 aeron::util::index_t length,
                                 const aeron::Header& /*header*/) {
    using namespace bifrost::protocol;

    if (length < static_cast<aeron::util::index_t>(MessageHeader::encodedLength())) return;

    char* data = const_cast<char*>(reinterpret_cast<const char*>(buffer.buffer() + offset));
    MessageHeader hdr;
    hdr.wrap(data, 0, MessageHeader::sbeSchemaVersion(), static_cast<uint64_t>(length));

    if (hdr.templateId() != ExecutionReport::sbeTemplateId()) return;

    ExecutionReport msg;
    msg.wrapForDecode(data,
                      MessageHeader::encodedLength(),
                      hdr.blockLength(),
                      hdr.version(),
                      static_cast<uint64_t>(length));

    const auto status = msg.status();
    const auto side = (msg.side() == OrderSide::BUY) ? encode::Side::Buy : encode::Side::Sell;
    const double price = static_cast<double>(msg.price()) / kPriceScale;

    // Fire order lifecycle event for all statuses so the dashboard can
    // track open/working orders.
    if (order_handler_) {
        OrderEvent ev{};
        ev.ts_ns         = msg.timestampNs();
        ev.order_id      = msg.orderId();
        ev.instrument_id = msg.instrumentId();
        ev.side          = side;
        ev.status        = static_cast<uint8_t>(status);
        ev.order_type    = msg.orderTypeRaw();
        ev.price         = price;
        ev.qty           = static_cast<double>(msg.filledQty() + msg.remainingQty()) / kQtyScale;
        ev.filled_qty    = static_cast<double>(msg.filledQty()) / kQtyScale;
        ev.remaining_qty = static_cast<double>(msg.remainingQty()) / kQtyScale;
        order_handler_(ev);
    }

    // Only forward real fills to the position tracker / equity curve.
    // OKX (and other venues) can emit PARTIAL exec reports with filled_qty=0
    // as order-state updates ("order is resting in book"). Those are not
    // actual executions and must not be counted as fills.
    if (status != ExecStatus::FILLED && status != ExecStatus::PARTIAL) return;
    if (msg.filledQty() == 0) return;

    Fill f{};
    f.ts_ns         = msg.timestampNs();
    f.order_id      = msg.orderId();
    f.instrument_id = msg.instrumentId();
    f.side          = side;
    f.order_type    = msg.orderTypeRaw();
    f.qty           = static_cast<double>(msg.filledQty()) / kQtyScale;
    f.price         = price;
    f.fee           = static_cast<double>(msg.fee()) / kFeeScale;

    if (handler_) handler_(f);
}

}  // namespace bridge

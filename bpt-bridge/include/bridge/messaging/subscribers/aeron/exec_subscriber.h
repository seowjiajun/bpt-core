#pragma once

/// @file
/// Aeron-backed ExecutionReport subscriber. CRTP-templated; dispatches
/// on_exec_order_event for all statuses, on_exec_fill only for real
/// fills (non-zero filled_qty and status is FILLED or PARTIAL).

#include "bridge/aeron/sbe_decode.h"
#include "bridge/messaging/subscribers/api/exec_subscriber.h"

#include <Aeron.h>

#include <messages/ExecStatus.h>
#include <messages/ExecutionReport.h>
#include <messages/OrderSide.h>

#include <bpt_common/aeron/subscriber.h>
#include <bpt_common/logging.h>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

namespace bpt::bridge::messaging::aeron {

template <class Handler>
class ExecSubscriber final : public api::ExecSubscriber {
public:
    ExecSubscriber(std::shared_ptr<::aeron::Aeron> aeron, const std::string& channel, int32_t stream_id) {
        sub_ = std::make_unique<bpt::common::aeron::Subscriber>(
            std::move(aeron),
            channel,
            stream_id,
            [this](::aeron::AtomicBuffer& b, ::aeron::util::index_t o, ::aeron::util::index_t l, ::aeron::Header& h) {
                on_fragment(b, o, l, h);
            });
        bpt::common::log::info("[bridge/Exec] subscribed on {} stream {}", channel, stream_id);
    }

    void set_handler(Handler* handler) noexcept { handler_ = handler; }

    int poll(int fragment_limit = 32) override { return sub_ ? sub_->poll(fragment_limit) : 0; }

private:
    void on_fragment(::aeron::AtomicBuffer& buffer,
                     ::aeron::util::index_t offset,
                     ::aeron::util::index_t length,
                     ::aeron::Header& /*header*/) {
        using bpt::messages::ExecStatus;
        using bpt::messages::OrderSide;

        decode_sbe_fragment<bpt::messages::ExecutionReport>(
            buffer, offset, length, [this](bpt::messages::ExecutionReport& msg) {
                constexpr double kPriceScale = 1e8;
                constexpr double kQtyScale = 1e8;
                constexpr double kFeeScale = 1e8;

                if (handler_ == nullptr) [[unlikely]]
                    return;

                const auto status = msg.status();
                const auto side = (msg.side() == OrderSide::BUY) ? encode::Side::Buy : encode::Side::Sell;
                const double price = static_cast<double>(msg.price()) / kPriceScale;

                // Fire order lifecycle event for all statuses so the console
                // can track open/working orders.
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
                handler_->on_exec_order_event(ev);

                // Only forward real fills. PARTIAL with filled_qty=0 is an
                // order-state update ("resting in book"), not an actual fill.
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

                handler_->on_exec_fill(f);
            });
    }

    std::unique_ptr<bpt::common::aeron::Subscriber> sub_;
    Handler* handler_{nullptr};
};

}  // namespace bpt::bridge::messaging::aeron

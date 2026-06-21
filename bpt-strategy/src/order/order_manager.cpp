#include "strategy/order/order_manager.h"

#include "strategy/refdata/instrument.h"

#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <bpt_common/logging.h>
#include <cmath>

using bpt::messages::ExecStatus;
using bpt::messages::OrderSide;
using bpt::messages::OrderType;

namespace bpt::strategy::order {

OrderManager::OrderManager(IOrderGatewayClient& gw, const refdata::InstrumentCache& cache) : gw_(gw), cache_(cache) {}

namespace {

// Normalise + validate. Returns true and writes normalised price/qty on
// success; false on any pre-flight rejection (already logged).
bool normalise_and_validate(const refdata::InstrumentCache& cache,
                            uint64_t instrument_id,
                            bpt::messages::OrderSide::Value side,
                            bpt::messages::OrderType::Value order_type,
                            double& price,
                            double& quantity,
                            std::string& exchange_symbol_out) {
    const auto inst = cache.get(instrument_id);
    if (!inst) {
        bpt::common::log::warn("[OrderMgr] Rejected: instrument_id={} not in refdata cache", instrument_id);
        return false;
    }
    if (inst->status != refdata::InstrumentStatus::ACTIVE) {
        bpt::common::log::warn("[OrderMgr] Rejected: instrument {} is not ACTIVE", inst->symbol);
        return false;
    }
    if (order_type != OrderType::MARKET && inst->tick_size > 0.0) {
        if (side == OrderSide::BUY)
            price = std::ceil(price / inst->tick_size) * inst->tick_size;
        else
            price = std::floor(price / inst->tick_size) * inst->tick_size;
    }
    if (inst->lot_size > 0.0) {
        quantity = std::floor(quantity / inst->lot_size) * inst->lot_size;
        if (quantity < inst->lot_size)
            quantity = inst->lot_size;
    }
    if (quantity <= 0.0) {
        bpt::common::log::warn("[OrderMgr] Rejected: quantity <= 0 after lot normalisation for {}", inst->symbol);
        return false;
    }
    if (order_type != OrderType::MARKET && price <= 0.0) {
        bpt::common::log::warn("[OrderMgr] Rejected: price <= 0 for non-MARKET order on {}", inst->symbol);
        return false;
    }
    exchange_symbol_out = inst->symbol;
    return true;
}

}  // namespace

OrderHandle OrderManager::send_quote(uint64_t instrument_id,
                                      bpt::messages::ExchangeId::Value exchange_id,
                                      bpt::messages::OrderSide::Value side,
                                      double price, double qty, uint8_t tag) {
    if (const auto inst = cache_.get(instrument_id); inst && inst->tick_size > 0.0)
        price = (side == OrderSide::BUY) ? std::floor(price / inst->tick_size) * inst->tick_size
                                         : std::ceil(price / inst->tick_size) * inst->tick_size;
    return send_new_order({.instrument_id = instrument_id,
                           .exchange_id = exchange_id,
                           .side = side,
                           .type = OrderType::LIMIT,
                           .tif = bpt::messages::TimeInForce::GTC,
                           .price = price,
                           .qty = qty,
                           .exec_inst = {.post_only = true}},
                          tag);
}

OrderHandle OrderManager::send_new_order(const NewOrderRequest& req, uint8_t tag) {
    double price = req.price;
    double quantity = req.qty;
    std::string symbol;
    if (!normalise_and_validate(cache_, req.instrument_id, req.side, req.type, price, quantity, symbol))
        return {};

    const int64_t price_fp = static_cast<int64_t>(std::round(price * 1e8));
    const uint64_t qty_fp = static_cast<uint64_t>(std::round(quantity * 1e8));
    const uint64_t order_id = next_order_id_.fetch_add(1, std::memory_order_relaxed);

    if (!gw_.send_new_order(OutboundNewOrder{
            .order_id = order_id,
            .exchange_id = req.exchange_id,
            .instrument_id = req.instrument_id,
            .side = req.side,
            .order_type = req.type,
            .tif = req.tif,
            .price = price_fp,
            .quantity = qty_fp,
            .exec_inst = req.exec_inst.to_bitmask(),
            .exchange_symbol = symbol,
        }))
        return {};

    const uint64_t now_ns = bpt::common::util::WallClock::now_ns();
    store_.push_back(OrderState{
        .order_id = order_id,
        .instrument_id = req.instrument_id,
        .exchange_id = req.exchange_id,
        .side = req.side,
        .price = price,
        .qty = quantity,
        .filled_qty = 0.0,
        .avg_fill_price = 0.0,
        .status = OrderState::Status::Live,
        .created_ns = now_ns,
        .last_update_ns = now_ns,
        .tag = tag,
    });
    OrderState* state = &store_.back();
    lookup_[order_id] = state;

    if (on_order_placed)
        on_order_placed(order_id);
    return OrderHandle{state};
}

void OrderManager::send_cancel(OrderHandle& handle) {
    if (!handle.live())
        return;  // terminal or already-pending — nothing to send
    OrderState* s = handle.state;
    // Set CancelPending BEFORE the gateway call: the in-process backtest
    // harness fires the CANCELLED ExecReport synchronously inside
    // send_cancel, which routes back through on_exec_report and sets
    // Status::Cancelled. If we set CancelPending after, we'd overwrite
    // the terminal Cancelled with CancelPending and the strategy would
    // never see the order terminate.
    s->status = OrderState::Status::CancelPending;
    s->last_update_ns = bpt::common::util::WallClock::now_ns();
    gw_.send_cancel(CancelOrderRequest{s->order_id, s->exchange_id, s->instrument_id});
}

void OrderManager::send_cancel(const CancelOrderRequest& req) {
    // Same pre-set-then-send ordering as the handle overload.
    if (auto it = lookup_.find(req.order_id); it != lookup_.end() && it->second->live()) {
        it->second->status = OrderState::Status::CancelPending;
        it->second->last_update_ns = bpt::common::util::WallClock::now_ns();
    }
    gw_.send_cancel(req);
}

void OrderManager::cancel_all(bpt::messages::ExchangeId::Value exchange_id, uint64_t instrument_id) {
    gw_.send_cancel_all(exchange_id, instrument_id);
}

void OrderManager::modify_order(const ModifyOrderRequest& req) {
    gw_.send_modify(req);
}

void OrderManager::on_exec_report(const bpt::messages::ExecutionReport& rpt) {
    auto it = lookup_.find(rpt.orderId());
    if (it == lookup_.end())
        return;  // not OM-tracked (e.g. deprecated place_order path)
    OrderState* s = it->second;
    s->last_update_ns = bpt::common::util::WallClock::now_ns();

    // filledQty / remainingQty are wire-format fixed-point 1e8.
    const double filled = static_cast<double>(rpt.filledQty()) / 1e8;
    if (filled > s->filled_qty) {
        // Crude running VWAP using submitted price as a stand-in until SBE
        // adds last_fill_price; Phase 3 polish.
        const double new_qty = filled - s->filled_qty;
        const double last_px = static_cast<double>(rpt.price()) / 1e8;
        if (s->filled_qty + new_qty > 0.0)
            s->avg_fill_price = (s->avg_fill_price * s->filled_qty + last_px * new_qty) / (s->filled_qty + new_qty);
        s->filled_qty = filled;
    }

    switch (rpt.status()) {
        case ExecStatus::ACKED:
            s->status = OrderState::Status::Live;
            break;
        case ExecStatus::PARTIAL:
            s->status = OrderState::Status::PartiallyFilled;
            break;
        case ExecStatus::FILLED:
            s->status = OrderState::Status::Filled;
            break;
        case ExecStatus::CANCELLED:
            s->status = OrderState::Status::Cancelled;
            break;
        case ExecStatus::REJECTED:
            s->status = OrderState::Status::Rejected;
            break;
        default:
            break;  // unrecognised status — leave OrderState as-is
    }
}

OrderHandle OrderManager::find_by_id(uint64_t order_id) {
    auto it = lookup_.find(order_id);
    return it == lookup_.end() ? OrderHandle{} : OrderHandle{it->second};
}

}  // namespace bpt::strategy::order

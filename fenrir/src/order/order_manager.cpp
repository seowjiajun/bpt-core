#include "fenrir/order/order_manager.h"

#include "fenrir/refdata/instrument.h"

#include <bifrost_protocol/OrderSide.h>
#include <bifrost_protocol/OrderType.h>

#include <cmath>
#include <spdlog/spdlog.h>

using bifrost::protocol::OrderSide;
using bifrost::protocol::OrderType;

namespace fenrir::order {

OrderManager::OrderManager(OrderGatewayClient& gw, const refdata::InstrumentCache& cache) : gw_(gw), cache_(cache) {}

uint64_t OrderManager::place_order(uint64_t instrument_id,
                                   bifrost::protocol::ExchangeId::Value exchange_id,
                                   bifrost::protocol::OrderSide::Value side,
                                   bifrost::protocol::OrderType::Value order_type,
                                   bifrost::protocol::TimeInForce::Value tif,
                                   double price,
                                   double quantity) {
    const auto inst = cache_.get(instrument_id);
    if (!inst) {
        spdlog::warn("[OrderMgr] Rejected: instrument_id={} not in refdata cache", instrument_id);
        return 0;
    }

    if (inst->status != refdata::InstrumentStatus::ACTIVE) {
        spdlog::warn("[OrderMgr] Rejected: instrument {} is not ACTIVE", inst->symbol);
        return 0;
    }

    // Normalise price to tick size.
    // BUY rounds up (ensures the order is at or above the required price level).
    // SELL rounds down (ensures the order is at or below).
    if (order_type != OrderType::MARKET && inst->tick_size > 0.0) {
        if (side == OrderSide::BUY)
            price = std::ceil(price / inst->tick_size) * inst->tick_size;
        else
            price = std::floor(price / inst->tick_size) * inst->tick_size;
    }

    // Normalise quantity to lot size and enforce the minimum of one lot.
    if (inst->lot_size > 0.0) {
        quantity = std::floor(quantity / inst->lot_size) * inst->lot_size;
        if (quantity < inst->lot_size)
            quantity = inst->lot_size;
    }

    if (quantity <= 0.0) {
        spdlog::warn("[OrderMgr] Rejected: quantity <= 0 after lot normalisation for {}", inst->symbol);
        return 0;
    }
    if (order_type != OrderType::MARKET && price <= 0.0) {
        spdlog::warn("[OrderMgr] Rejected: price <= 0 for non-MARKET order on {}", inst->symbol);
        return 0;
    }

    const int64_t price_fp = static_cast<int64_t>(std::round(price * 1e8));
    const uint64_t qty_fp = static_cast<uint64_t>(std::round(quantity * 1e8));

    const uint64_t order_id = next_order_id_.fetch_add(1, std::memory_order_relaxed);

    if (!gw_.send_new_order(order_id,
                            exchange_id,
                            instrument_id,
                            side,
                            order_type,
                            tif,
                            price_fp,
                            qty_fp,
                            inst->symbol))
        return 0;

    if (on_order_placed)
        on_order_placed(order_id);

    return order_id;
}

void OrderManager::cancel_order(uint64_t order_id,
                                bifrost::protocol::ExchangeId::Value exchange_id,
                                uint64_t instrument_id) {
    gw_.send_cancel(order_id, exchange_id, instrument_id);
}

void OrderManager::cancel_all(bifrost::protocol::ExchangeId::Value exchange_id, uint64_t instrument_id) {
    gw_.send_cancel_all(exchange_id, instrument_id);
}

void OrderManager::modify_order(uint64_t order_id,
                                bifrost::protocol::ExchangeId::Value exchange_id,
                                uint64_t instrument_id,
                                int64_t new_price,
                                uint64_t new_quantity) {
    gw_.send_modify(order_id, exchange_id, instrument_id, new_price, new_quantity);
}

}  // namespace fenrir::order

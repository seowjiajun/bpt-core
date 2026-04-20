#include "strategy/order/order_manager.h"

#include "strategy/refdata/instrument.h"

#include <messages/OrderSide.h>
#include <messages/OrderType.h>

#include <cmath>
#include <bpt_common/logging.h>

using bpt::messages::OrderSide;
using bpt::messages::OrderType;

namespace bpt::strategy::order {

OrderManager::OrderManager(IOrderGatewayClient& gw, const refdata::InstrumentCache& cache) : gw_(gw), cache_(cache) {}

uint64_t OrderManager::place_order(uint64_t instrument_id,
                                   bpt::messages::ExchangeId::Value exchange_id,
                                   bpt::messages::OrderSide::Value side,
                                   bpt::messages::OrderType::Value order_type,
                                   bpt::messages::TimeInForce::Value tif,
                                   double price,
                                   double quantity) {
    const auto inst = cache_.get(instrument_id);
    if (!inst) {
        bpt::common::log::warn("[OrderMgr] Rejected: instrument_id={} not in refdata cache", instrument_id);
        return 0;
    }

    if (inst->status != refdata::InstrumentStatus::ACTIVE) {
        bpt::common::log::warn("[OrderMgr] Rejected: instrument {} is not ACTIVE", inst->symbol);
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
        bpt::common::log::warn("[OrderMgr] Rejected: quantity <= 0 after lot normalisation for {}", inst->symbol);
        return 0;
    }
    if (order_type != OrderType::MARKET && price <= 0.0) {
        bpt::common::log::warn("[OrderMgr] Rejected: price <= 0 for non-MARKET order on {}", inst->symbol);
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
                                bpt::messages::ExchangeId::Value exchange_id,
                                uint64_t instrument_id) {
    gw_.send_cancel(order_id, exchange_id, instrument_id);
}

void OrderManager::cancel_all(bpt::messages::ExchangeId::Value exchange_id, uint64_t instrument_id) {
    gw_.send_cancel_all(exchange_id, instrument_id);
}

void OrderManager::modify_order(uint64_t order_id,
                                bpt::messages::ExchangeId::Value exchange_id,
                                uint64_t instrument_id,
                                int64_t new_price,
                                uint64_t new_quantity) {
    gw_.send_modify(order_id, exchange_id, instrument_id, new_price, new_quantity);
}

}  // namespace bpt::strategy::order

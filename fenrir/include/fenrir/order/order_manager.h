#pragma once

#include "fenrir/order/order_gateway_client.h"
#include "fenrir/refdata/instrument_cache.h"

#include <bifrost_protocol/ExchangeId.h>
#include <bifrost_protocol/OrderSide.h>
#include <bifrost_protocol/OrderType.h>
#include <bifrost_protocol/TimeInForce.h>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace fenrir::order {

// OrderManager is the single point through which all strategies place orders.
// It centralises:
//   - Globally unique order ID generation (previously one atomic per strategy).
//   - Instrument validation: status must be ACTIVE, instrument must exist in cache.
//   - Price normalisation: rounded to tick_size (BUY rounds up, SELL rounds down).
//   - Quantity normalisation: rounded down to lot_size, clamped to minimum one lot.
//   - Exchange symbol resolution: fetched from the refdata cache — strategies never
//     need to carry the exchange-native symbol themselves.
//   - Final gate to OrderGatewayClient::send_new_order().
//
// Usage:
//   uint64_t order_id = order_mgr_.place_order(instrument_id, exchange_id,
//                                               side, type, tif, price, qty);
//   if (order_id != 0) {
//       // order was sent — record state
//   }
//
// price and quantity are passed in natural units (e.g. 70000.0, 0.001).
// Internal fixed-point conversion (1e8 scale) is handled here.
//
// Threading: single-threaded — must be called from the Aeron poll thread only.
class OrderManager {
public:
    OrderManager(OrderGatewayClient& gw, const refdata::InstrumentCache& cache);

    // Place a new order.  Returns the assigned order_id (> 0) on success, or 0
    // if any pre-flight check fails (instrument unknown/inactive, zero quantity,
    // non-positive price for a LIMIT order, etc.).
    [[nodiscard]] uint64_t place_order(uint64_t instrument_id,
                                       bifrost::protocol::ExchangeId::Value exchange_id,
                                       bifrost::protocol::OrderSide::Value side,
                                       bifrost::protocol::OrderType::Value order_type,
                                       bifrost::protocol::TimeInForce::Value tif,
                                       double price,
                                       double quantity);

    // Direct access to the underlying gateway — use sparingly (prefer the methods above).
    // Provided for legacy strategies not yet migrated to OrderManager.
    OrderGatewayClient& gw() { return gw_; }

    // Thin delegations to the underlying gateway — strategies should use these
    // rather than holding a raw OrderGatewayClient pointer.
    void cancel_order(uint64_t order_id, bifrost::protocol::ExchangeId::Value exchange_id, uint64_t instrument_id);

    void cancel_all(bifrost::protocol::ExchangeId::Value exchange_id, uint64_t instrument_id);

    // Modify uses wire-format (1e8) fixed-point price and quantity — caller is responsible
    // for converting from natural units if needed.
    void modify_order(uint64_t order_id,
                      bifrost::protocol::ExchangeId::Value exchange_id,
                      uint64_t instrument_id,
                      int64_t new_price,
                      uint64_t new_quantity);

private:
    OrderGatewayClient& gw_;
    const refdata::InstrumentCache& cache_;
    // High 32 bits = Unix timestamp at construction (seconds), low 32 bits = counter.
    // Guarantees uniqueness across process restarts without any persistent state.
    static uint64_t make_session_base() {
        const uint64_t ts = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        return ts << 32;
    }
    std::atomic<uint64_t> next_order_id_{make_session_base() + 1};
};

}  // namespace fenrir::order

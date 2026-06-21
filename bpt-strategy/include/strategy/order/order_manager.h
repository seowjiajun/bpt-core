#pragma once

#include "bpt_common/util/tsc_clock.h"
#include "strategy/order/i_order_gateway_client.h"
#include "strategy/order/order_handle.h"
#include "strategy/order/requests.h"
#include "strategy/refdata/instrument_cache.h"

#include <messages/ExchangeId.h>
#include <messages/ExecutionReport.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <unordered_map>

namespace bpt::strategy::order {

// OrderManager is the single point through which all strategies place orders.
// It centralises:
//   - Globally unique order ID generation (previously one atomic per strategy).
//   - Instrument validation: status must be ACTIVE, instrument must exist in cache.
//   - Price normalisation: rounded to tick_size (BUY rounds up, SELL rounds down).
//   - Quantity normalisation: rounded down to lot_size, clamped to minimum one lot.
//   - Exchange symbol resolution: fetched from the refdata cache — strategies never
//     need to carry the exchange-native symbol themselves.
//   - Final gate to IOrderGatewayClient::send_new_order().
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
    OrderManager(IOrderGatewayClient& gw, const refdata::InstrumentCache& cache);

    // Typed entry points (mlp-algo-style). send_new_order tracks lifecycle
    // and returns a handle that stays valid for the process lifetime.
    // `tag` is strategy-defined intent (e.g. Quote vs Unwind); OM does
    // not interpret it. Returns an invalid handle on validation failure.
    [[nodiscard]] OrderHandle send_new_order(const NewOrderRequest& req, uint8_t tag = 0);
    void send_cancel(OrderHandle& handle);
    void send_cancel(const CancelOrderRequest& req);

    // Strategy forwards every ExecutionReport here so OM can update the
    // matching OrderState before strategy-level dispatch runs.
    void on_exec_report(const bpt::messages::ExecutionReport& rpt);

    // Lookup the live or terminal record by exchange-assigned order_id.
    // Returns invalid handle if unknown.
    [[nodiscard]] OrderHandle find_by_id(uint64_t order_id);

    // Called by StrategyService to measure MD-to-order latency. Set before strategy callbacks.
    // Invoked synchronously inside place_order on success, before returning to the caller.
    std::function<void(uint64_t order_id)> on_order_placed;

    // Direct access to the underlying gateway — use sparingly (prefer the methods above).
    // Provided for legacy strategies not yet migrated to OrderManager.
    IOrderGatewayClient& gw() { return gw_; }

    // Thin delegations to the underlying gateway — strategies should use these
    // rather than holding a raw IOrderGatewayClient pointer.
    void cancel_all(bpt::messages::ExchangeId::Value exchange_id, uint64_t instrument_id);

    // Modify uses wire-format (1e8) fixed-point price and quantity (carried
    // on ModifyOrderRequest) — caller converts from natural units if needed.
    void modify_order(const ModifyOrderRequest& req);

private:
    // Live + historical order records. deque preserves stable pointers
    // under push_back; lookup_ holds raw pointers into the deque keyed by
    // exchange-assigned order_id. No pruning yet (Phase 3 concern).
    std::deque<OrderState> store_;
    std::unordered_map<uint64_t, OrderState*> lookup_;

    IOrderGatewayClient& gw_;
    const refdata::InstrumentCache& cache_;
    // High 32 bits = Unix timestamp at construction (seconds), low 32 bits = counter.
    // Guarantees uniqueness across process restarts without any persistent state.
    //
    // Env var BPT_ORDER_ID_SEED overrides the wallclock seed for
    // deterministic-replay tests (see deterministic backtester parity
    // suite). Without it, two replays of the same input produce different
    // order_id integers — fine for production but hides a class of
    // matching-engine non-determinism bugs in tests.
    static uint64_t make_session_base() {
        if (const char* s = std::getenv("BPT_ORDER_ID_SEED"))
            return static_cast<uint64_t>(std::strtoull(s, nullptr, 10)) << 32;
        return bpt::common::util::WallClock::now_s() << 32;
    }
    std::atomic<uint64_t> next_order_id_{make_session_base() + 1};
};

}  // namespace bpt::strategy::order

#include "order_gateway/adapter/hyperliquid/hyperliquid_exec_decoder.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <bpt_common/logging.h>
#include <string>

namespace bpt::order_gateway::adapter {

namespace json = boost::json;

static constexpr double kScale = 1e8;

void HyperliquidExecDecoder::register_order(uint64_t exch_oid, uint64_t client_order_id, uint64_t original_qty_e8) {
    if (exch_oid == 0)
        return;
    pending_[exch_oid] = PendingOrder{client_order_id, original_qty_e8, 0};
}

void HyperliquidExecDecoder::handle_fills(const json::array& fills, uint64_t recv_ns) {
    for (const auto& fill_val : fills) {
        const auto& fill = fill_val.as_object();

        ExecEvent ev{};
        ev.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
        ev.local_ts_ns = recv_ns;

        ev.exchange_order_id = static_cast<uint64_t>(fill.at("oid").as_int64());

        // Resolve client_order_id via the pending map. HL does not echo a
        // cloid in userFills (we don't send one), so this lookup is the only
        // way to route a fill back to the originating strategy order.
        auto it = pending_.find(ev.exchange_order_id);
        if (it == pending_.end()) [[unlikely]] {
            // Unknown oid. Possible reasons: fill arrived before the order
            // ack response (rare, same WS channel so ordering should hold);
            // fill for an order from a previous order-gateway session; or a fill
            // for an order we never placed. Skip.
            continue;
        }

        PendingOrder& po = it->second;
        ev.order_id = po.client_order_id;
        ev.instrument_id = 0;  // fenrir resolves via its own order_to_instrument_ map

        std::string side_str = std::string(fill.at("side").as_string());
        ev.side = (side_str == "B") ? bpt::messages::OrderSide::BUY : bpt::messages::OrderSide::SELL;
        ev.order_type = bpt::messages::OrderType::LIMIT;

        ev.price = static_cast<int64_t>(std::stod(std::string(fill.at("px").as_string())) * kScale);
        const uint64_t slice_qty_e8 = static_cast<uint64_t>(std::stod(std::string(fill.at("sz").as_string())) * kScale);
        ev.filled_qty = slice_qty_e8;

        po.cumulative_filled_e8 += slice_qty_e8;
        const bool fully_filled = po.cumulative_filled_e8 >= po.original_qty_e8;

        ev.remaining_qty = fully_filled ? 0 : (po.original_qty_e8 - po.cumulative_filled_e8);

        ev.fee = static_cast<int64_t>(std::stod(std::string(fill.at("fee").as_string())) * kScale);
        ev.fee_currency = "USDT";
        ev.reject_reason = bpt::messages::RejectReason::OK;
        ev.status = fully_filled ? bpt::messages::ExecStatus::FILLED : bpt::messages::ExecStatus::PARTIAL;

        if (auto ts_it = fill.find("time"); ts_it != fill.end())
            ev.exchange_ts_ns = static_cast<uint64_t>(ts_it->value().as_int64()) * 1000000ULL;
        else
            ev.exchange_ts_ns = recv_ns;

        if (on_exec_event)
            on_exec_event(ev);

        if (fully_filled) {
            pending_.erase(it);
        }
    }
}

}  // namespace bpt::order_gateway::adapter

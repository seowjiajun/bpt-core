#pragma once

// OKX WebSocket action JSON builders — pure input → JSON, no state,
// no I/O. Each helper produces a boost::json::value in exactly the
// shape OKX's trading WebSocket (wss://ws.okx.com:8443/ws/v5/private)
// accepts under `op:"order" / "cancel-order" / "amend-order"`.
//
// The OKX order WS is fire-and-forget — unlike Hyperliquid where the
// action codec is paired with a synchronous post-action client, OKX
// posts the message and consumes acks/fills asynchronously through the
// `orders` subscription channel. So these builders never need to know
// about nonces or signatures; the WS is already authenticated at
// login time.
//
// Contract-size and instIdCode tables are passed in by the caller
// rather than owned by the codec — they live on the
// OKXInstrumentsService at the adapter level and changing one
// doesn't force a codec rebuild.

#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/TimeInForce.h>

#include <boost/json/fwd.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bpt::order_gateway::adapter::okx {

// Lookup tables the codec needs. Keys are OKX `instId` strings
// ("BTC-USDT", "BTC-USDT-SWAP", etc.).
//   inst_id_codes  — /api/v5/public/instruments → instIdCode field,
//                    required by the WS `order` op.
//   contract_sizes — /api/v5/public/instruments → ctVal field; 1.0 for
//                    SPOT/MARGIN, >1 for SWAP/FUTURES. Used to convert
//                    fenrir's base-currency qty into OKX's sz (contracts).
using InstIdCodeMap  = std::unordered_map<std::string, int64_t>;
using ContractSizes  = std::unordered_map<std::string, double>;

// Common parameters shared by build_order_action / build_modify_action.
struct OrderSpec {
    std::string                        inst_id;         // e.g. "BTC-USDT-SWAP"
    bpt::messages::OrderSide::Value side;
    bpt::messages::OrderType::Value order_type;
    bpt::messages::TimeInForce::Value tif;
    int64_t                             price_e8;        // natural * 1e8
    uint64_t                            quantity_e8;     // base currency * 1e8
    std::string                         cloid;           // client order id
};

// Build the `{"id":...,"op":"order","args":[{...}]}` envelope for a
// new order. req_id is a monotonic counter for matching the WS ack
// back to the adapter's pending-request tracking (OKX echoes it as
// the `id` field in the ack).
[[nodiscard]] boost::json::value build_order_action(const OrderSpec& spec,
                                                    uint64_t req_id,
                                                    const InstIdCodeMap& inst_id_codes,
                                                    const ContractSizes& contract_sizes);

// Build the `{"id":...,"op":"cancel-order","args":[...]}` envelope.
[[nodiscard]] boost::json::value build_cancel_action(std::string_view inst_id,
                                                     std::string_view cloid,
                                                     uint64_t req_id);

// Build the `{"op":"amend-order","args":[...]}` envelope. OKX's amend
// endpoint doesn't require an id field in the envelope — exec reports
// for the amended order come through the same `orders` channel as
// everything else.
[[nodiscard]] boost::json::value build_modify_action(std::string_view inst_id,
                                                     std::string_view cloid,
                                                     int64_t new_price_e8,
                                                     uint64_t new_quantity_e8,
                                                     const ContractSizes& contract_sizes);

}  // namespace bpt::order_gateway::adapter::okx

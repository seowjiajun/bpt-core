#include "backtester/harness/inprocess_order_gateway_client.h"

#include <messages/AccountSnapshot.h>
#include <messages/MessageHeader.h>
#include <messages/RejectReason.h>
#include <messages/RejectSource.h>

#include <algorithm>
#include <bpt_common/logging.h>
#include <cstring>
#include <vector>

namespace bpt::backtester::harness {

namespace {

// SBE-side scaling — mirrors strategy/order-gateway constants. Strategy
// passes int64 price = real_price * 1e8, uint64 qty = real_qty * 1e8.
constexpr double kPriceScale = 1e8;
constexpr double kQtyScale = 1e8;

constexpr uint8_t kExecInstPostOnlyMask = 0x01;

matching::OrderSide to_match_side(bpt::messages::OrderSide::Value s) {
    return s == bpt::messages::OrderSide::BUY ? matching::OrderSide::BUY : matching::OrderSide::SELL;
}

bpt::messages::OrderSide::Value to_msg_side(matching::OrderSide s) {
    return s == matching::OrderSide::BUY ? bpt::messages::OrderSide::BUY : bpt::messages::OrderSide::SELL;
}

matching::OrderType to_match_type(bpt::messages::OrderType::Value t, bool post_only) {
    if (post_only)
        return matching::OrderType::POST_ONLY;
    if (t == bpt::messages::OrderType::MARKET)
        return matching::OrderType::MARKET;
    return matching::OrderType::LIMIT;
}

bpt::messages::OrderType::Value to_msg_type(matching::OrderType t) {
    switch (t) {
        case matching::OrderType::MARKET:
            return bpt::messages::OrderType::MARKET;
        case matching::OrderType::LIMIT:
            return bpt::messages::OrderType::LIMIT;
        case matching::OrderType::POST_ONLY:
            return bpt::messages::OrderType::LIMIT;  // wire-level
    }
    return bpt::messages::OrderType::LIMIT;
}

}  // namespace

std::string InProcessOrderGatewayClient::exchange_id_string(bpt::messages::ExchangeId::Value id) {
    using EX = bpt::messages::ExchangeId;
    switch (id) {
        case EX::BINANCE:
            return "BINANCE";
        case EX::OKX:
            return "OKX";
        case EX::HYPERLIQUID:
            return "HYPERLIQUID";
        case EX::DERIBIT:
            return "DERIBIT";
        default:
            return "";
    }
}

InProcessOrderGatewayClient::InProcessOrderGatewayClient(matching::MatchingEngine& matching) : matching_(matching) {
    matching_.set_fill_callback([this](matching::FillReport fr) { publish_fill(fr); });
}

bool InProcessOrderGatewayClient::send_new_order(uint64_t order_id,
                                                 bpt::messages::ExchangeId::Value exchange_id,
                                                 uint64_t instrument_id,
                                                 bpt::messages::OrderSide::Value side,
                                                 bpt::messages::OrderType::Value order_type,
                                                 bpt::messages::TimeInForce::Value tif,
                                                 int64_t price,
                                                 uint64_t quantity,
                                                 uint8_t exec_inst,
                                                 const std::string& exchange_symbol) {
    if (quantity == 0)
        return false;
    if (order_type != bpt::messages::OrderType::MARKET && price <= 0)
        return false;
    if (exchange_symbol.empty())
        return false;

    const bool post_only = (exec_inst & kExecInstPostOnlyMask) != 0;
    const std::string exchange_str = exchange_id_string(exchange_id);

    LiveOrder lo{
        .strategy_order_id = order_id,
        .exchange_id = exchange_id,
        .instrument_id = instrument_id,
        .side = side,
        .order_type = order_type,
        .tif = tif,
        .price = price,
        .quantity = quantity,
        .exchange_symbol = exchange_symbol,
        .exchange = exchange_str,
        .post_only = post_only,
    };
    live_.emplace(order_id, lo);

    matching::OpenOrder mo{
        .order_id = std::to_string(order_id),
        .client_order_id = std::to_string(order_id),
        .exchange = exchange_str,
        .symbol = exchange_symbol,
        .type = to_match_type(order_type, post_only),
        .side = to_match_side(side),
        .price = static_cast<double>(price) / kPriceScale,
        .quantity = static_cast<double>(quantity) / kQtyScale,
        .filled_qty = 0.0,
        .submitted_ts = simulation_now_ns_,
    };

    matching::OpenOrder result = matching_.submit_order(std::move(mo));

    if (result.rejected) {
        // POST_ONLY would have crossed → matching declines. Synchronous
        // single-process semantics make a fired-during-submit
        // ExecReport unsafe: AS strategy's on_exec_report runs INSIDE
        // place_order before place_order returns, so the strategy's
        // own bid_order_id / ask_order_id assignment hasn't happened
        // yet. The REJECTED handler can't clear something that hasn't
        // been set, and place_order then sets bid_order_id to a dead
        // order id — the strategy waits forever for a terminal status
        // that already fired.
        //
        // The clean local fix: return false. The strategy's place_order
        // returns 0, bid_order_id stays 0, strategy retries next tick.
        // This treats POST_ONLY-cross like a pre-trade risk reject —
        // no order was ever placed from the strategy's point of view.
        //
        // Counter logged to keep the operator aware of how often quotes
        // are crossing the book on submit.
        ++rejected_count_;
        if (rejected_count_ <= 5 || rejected_count_ % 100 == 0) {
            bpt::common::log::warn("[harness-OGW] POST_ONLY rejected (would cross): {} {} {} @ {} (count={})",
                                   exchange_str,
                                   exchange_symbol,
                                   side == bpt::messages::OrderSide::BUY ? "BUY" : "SELL",
                                   static_cast<double>(price) / kPriceScale,
                                   rejected_count_);
        }
        live_.erase(order_id);
        return false;
    }

    // Synchronous MARKET / crossing-LIMIT fills already fired their
    // FillReports through publish_fill before submit_order returned.
    // Anything still resting deserves an ACKED to drive the strategy's
    // on_exec_report ACKED branch.
    auto it = live_.find(order_id);
    if (it != live_.end()) {
        const uint64_t cumfilled = it->second.cumulative_filled_qty;
        if (cumfilled < quantity) {
            publish_exec_status(order_id,
                                exchange_id,
                                instrument_id,
                                bpt::messages::ExecStatus::ACKED,
                                side,
                                order_type,
                                price,
                                quantity,
                                cumfilled);
        }
    }
    return true;
}

void InProcessOrderGatewayClient::send_cancel(uint64_t order_id,
                                              bpt::messages::ExchangeId::Value exchange_id,
                                              uint64_t instrument_id) {
    auto it = live_.find(order_id);
    if (it == live_.end())
        return;
    const auto& lo = it->second;

    const bool ok = matching_.cancel_order(lo.exchange, lo.exchange_symbol, std::to_string(order_id));
    if (ok) {
        publish_exec_status(order_id,
                            exchange_id,
                            instrument_id,
                            bpt::messages::ExecStatus::CANCELLED,
                            lo.side,
                            lo.order_type,
                            lo.price,
                            lo.quantity,
                            lo.cumulative_filled_qty);
        live_.erase(it);
    }
    // Engine's cancel_order returns false when the order has already
    // fully filled or been canceled — strategy treated the order as
    // live but the engine doesn't know about it. Silent — matches
    // production behavior where an exchange "cancel rejected" is rare
    // enough to be ignored at this layer.
}

void InProcessOrderGatewayClient::send_cancel_all(bpt::messages::ExchangeId::Value exchange_id,
                                                  uint64_t instrument_id) {
    // Snapshot the live-order ids before iterating: send_cancel mutates
    // live_ via erase().
    std::vector<uint64_t> targets;
    targets.reserve(live_.size());
    for (const auto& [oid, lo] : live_) {
        if (lo.exchange_id == exchange_id && lo.instrument_id == instrument_id)
            targets.push_back(oid);
    }
    for (auto oid : targets) {
        send_cancel(oid, exchange_id, instrument_id);
    }
}

void InProcessOrderGatewayClient::send_modify(uint64_t order_id,
                                              bpt::messages::ExchangeId::Value /*exchange_id*/,
                                              uint64_t /*instrument_id*/,
                                              int64_t new_price,
                                              uint64_t new_quantity) {
    auto it = live_.find(order_id);
    if (it == live_.end())
        return;
    auto& lo = it->second;

    // Production HL `modify` semantics: venue-side in-place price/qty
    // change preserving order_id, no CANCELLED ExecReport emitted.
    // Strategy's bid_order_id / ask_order_id stays unchanged.
    //
    // MatchingEngine has no native modify, so we simulate by
    // cancelling the prior pending entry and resubmitting under the
    // SAME order_id — but crucially WITHOUT firing the CANCELLED
    // ExecReport that send_cancel() would normally emit. The strategy
    // sees the order remain live; only its parameters changed.
    matching_.cancel_order(lo.exchange, lo.exchange_symbol, std::to_string(order_id));

    lo.price = new_price;
    lo.quantity = new_quantity;

    matching::OpenOrder mo{
        .order_id = std::to_string(order_id),
        .client_order_id = std::to_string(order_id),
        .exchange = lo.exchange,
        .symbol = lo.exchange_symbol,
        .type = lo.post_only ? matching::OrderType::POST_ONLY : matching::OrderType::LIMIT,
        .side = lo.side == bpt::messages::OrderSide::BUY ? matching::OrderSide::BUY : matching::OrderSide::SELL,
        .price = static_cast<double>(new_price) / kPriceScale,
        .quantity = static_cast<double>(new_quantity) / kQtyScale,
        .filled_qty = static_cast<double>(lo.cumulative_filled_qty) / kQtyScale,
        .submitted_ts = simulation_now_ns_,
    };
    matching::OpenOrder result = matching_.submit_order(std::move(mo));

    // If the modified order would now cross (POST_ONLY rejection on a
    // bumped price that's into the book), the engine flags rejected.
    // Fire CANCELLED in that case — strategy treats it as the order
    // ending, the natural outcome for a cross-on-modify on HL Alo.
    if (result.rejected) {
        publish_exec_status(order_id,
                            lo.exchange_id,
                            lo.instrument_id,
                            bpt::messages::ExecStatus::CANCELLED,
                            lo.side,
                            lo.order_type,
                            new_price,
                            new_quantity,
                            lo.cumulative_filled_qty);
        live_.erase(it);
    }
}

void InProcessOrderGatewayClient::send_account_snapshot_request(bpt::messages::ExchangeId::Value /*exchange_id*/,
                                                                uint64_t /*correlation_id*/) {
    // Synthesise an empty AccountSnapshot. The harness can later be
    // extended to track positions internally and fill the snapshot's
    // positions[] group from there. For now AS strategy uses
    // AccountSnapshot at shutdown for reconciliation only — an empty
    // snapshot resets its tracker to "exchange thinks zero", which is
    // the correct state at end-of-simulation when fills have all flushed.
    if (!on_account_snapshot)
        return;
    constexpr std::size_t kBufSize =
        bpt::messages::MessageHeader::encodedLength() + bpt::messages::AccountSnapshot::sbeBlockLength();
    char buf[kBufSize]{};
    bpt::messages::AccountSnapshot snap;
    snap.wrapAndApplyHeader(buf, 0, kBufSize);
    on_account_snapshot(snap);
}

void InProcessOrderGatewayClient::set_simulation_time(uint64_t now_ns) {
    simulation_now_ns_ = now_ns;
}

void InProcessOrderGatewayClient::push_heartbeat() {
    last_heartbeat_ns_ = simulation_now_ns_;
    if (!on_heartbeat)
        return;
    constexpr std::size_t kBufSize =
        bpt::messages::MessageHeader::encodedLength() + bpt::messages::OrderGatewayHeartbeat::sbeBlockLength();
    char buf[kBufSize]{};
    bpt::messages::OrderGatewayHeartbeat hb;
    hb.wrapAndApplyHeader(buf, 0, kBufSize).timestampNs(simulation_now_ns_);
    on_heartbeat(hb);
}

void InProcessOrderGatewayClient::on_market_event_complete() {
    // No-op today; the matching engine fires fills synchronously inside
    // its own on_market_event(). Kept as a hook for future extension
    // (e.g., a tick-end batch flush of cancel-replace pairs).
}

void InProcessOrderGatewayClient::publish_fill(const matching::FillReport& fr) {
    uint64_t order_id = 0;
    try {
        order_id = std::stoull(fr.order_id);
    } catch (...) {
        return;
    }

    auto it = live_.find(order_id);
    if (it == live_.end()) {
        bpt::common::log::warn("[harness-OGW] fill for unknown order_id={} ({}/{} @ {})",
                               fr.order_id,
                               fr.exchange,
                               fr.symbol,
                               fr.last_fill_price);
        return;
    }
    bpt::common::log::info("[harness-OGW] FILL order_id={} {} {} @ {} qty={} ({})",
                           order_id,
                           fr.exchange,
                           fr.symbol,
                           fr.last_fill_price,
                           fr.last_fill_qty,
                           fr.liquidity_role == matching::LiquidityRole::MAKER ? "MAKER" : "TAKER");
    auto& lo = it->second;

    const uint64_t fill_qty_scaled = static_cast<uint64_t>(fr.last_fill_qty * kQtyScale + 0.5);
    const int64_t fill_px_scaled = static_cast<int64_t>(fr.last_fill_price * kPriceScale + 0.5);
    lo.cumulative_filled_qty += fill_qty_scaled;

    const auto status = fr.is_fully_filled ? bpt::messages::ExecStatus::FILLED : bpt::messages::ExecStatus::PARTIAL;

    publish_exec_status(order_id,
                        lo.exchange_id,
                        lo.instrument_id,
                        status,
                        to_msg_side(fr.side),
                        to_msg_type(fr.order_type),
                        fill_px_scaled,
                        fill_qty_scaled,
                        lo.cumulative_filled_qty);

    if (fr.is_fully_filled) {
        live_.erase(it);
    }
}

void InProcessOrderGatewayClient::publish_exec_status(uint64_t order_id,
                                                      bpt::messages::ExchangeId::Value exchange_id,
                                                      uint64_t instrument_id,
                                                      bpt::messages::ExecStatus::Value status,
                                                      bpt::messages::OrderSide::Value side,
                                                      bpt::messages::OrderType::Value order_type,
                                                      int64_t price,
                                                      uint64_t quantity,
                                                      uint64_t cumulative_filled_qty,
                                                      bpt::messages::RejectSource::Value reject_source) {
    if (!on_exec_report)
        return;

    using namespace bpt::messages;
    constexpr std::size_t kBufSize = MessageHeader::encodedLength() + ExecutionReport::sbeBlockLength();
    char buf[kBufSize]{};
    char ccy_pad[8] = {0};

    ExecutionReport rpt;
    rpt.wrapAndApplyHeader(buf, 0, kBufSize)
        .orderId(order_id)
        .exchangeOrderId(order_id)
        .exchangeId(exchange_id)
        .instrumentId(instrument_id)
        .status(status)
        .side(side)
        .orderType(order_type)
        .price(price)
        .filledQty(status == ExecStatus::FILLED || status == ExecStatus::PARTIAL ? quantity : 0)
        .remainingQty(quantity > cumulative_filled_qty ? (quantity - cumulative_filled_qty) : 0)
        .rejectReason(status == ExecStatus::REJECTED ? RejectReason::INVALID_PRICE : RejectReason::OK)
        .rejectSource(reject_source)
        .fee(0)
        .putFeeCurrency(ccy_pad)
        .timestampNs(simulation_now_ns_)
        .localTsNs(simulation_now_ns_);

    ++exec_seq_;
    on_exec_report(rpt);
}

}  // namespace bpt::backtester::harness

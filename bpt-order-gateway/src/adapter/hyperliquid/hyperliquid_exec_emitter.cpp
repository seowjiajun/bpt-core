#include "order_gateway/adapter/hyperliquid/hyperliquid_exec_emitter.h"

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <cmath>
#include <bpt_common/logging.h>
#include <bpt_common/util/tsc_clock.h>

namespace bpt::order_gateway::adapter::hyperliquid {

namespace json = boost::json;
using ES = bpt::messages::ExecStatus;
using RR = bpt::messages::RejectReason;
using FC = bpt::messages::FeeCurrency;

namespace {
constexpr double kScale = 1e8;

// Build the skeleton of an ExecEvent with everything the OrderContext
// knows. Callers fill in status + exch_oid + filled_qty + remaining_qty
// and push.
ExecEvent make_skeleton(const OrderContext& ctx, uint64_t now_ns) {
    ExecEvent ev{};
    ev.order_id      = ctx.client_order_id;
    ev.exchange_id   = bpt::messages::ExchangeId::HYPERLIQUID;
    ev.instrument_id = ctx.instrument_id;
    ev.side          = ctx.side;
    ev.order_type    = ctx.order_type;
    ev.price         = ctx.price_e8;
    ev.fee           = 0;
    ev.fee_currency  = FC::USDT;
    ev.exchange_ts_ns = now_ns;
    ev.local_ts_ns    = now_ns;
    return ev;
}

void push_or_log(util::ExecEventQueue& queue, const ExecEvent& ev, const char* tag) {
    if (!queue.try_push(ev))
        bpt::common::log::error("[Hyperliquid] exec_queue full — dropped {} ExecEvent", tag);
}

}  // namespace

bool HyperliquidExecEmitter::emit_order_response(const std::string& resp,
                                                 const OrderContext& ctx,
                                                 const std::function<void(uint64_t)>& on_acked) {
    try {
        const auto rj = json::parse(resp).as_object();
        const std::string status =
            rj.contains("status") ? std::string(rj.at("status").as_string()) : "";

        const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

        if (status != "ok") {
            bpt::common::log::warn("[OrderGateway] HL emitter: order rejected, status={}", status);
            ExecEvent ev = make_skeleton(ctx, now_ns);
            ev.status = ES::REJECTED;
            ev.reject_reason = RR::EXCHANGE_ERROR;
            ev.remaining_qty = ctx.quantity_e8;
            push_or_log(queue_, ev, "REJECTED");
            return true;
        }

        // status=ok — drill into response.data.statuses[0]
        if (!rj.contains("response")) return false;
        const auto& response = rj.at("response").as_object();
        if (!response.contains("data")) return false;
        const auto& data = response.at("data").as_object();
        if (!data.contains("statuses") || !data.at("statuses").is_array()) return false;
        const auto& statuses = data.at("statuses").as_array();
        if (statuses.empty()) return false;

        const auto& s0 = statuses[0].as_object();

        if (s0.contains("resting")) {
            // ACKED — order rests in the book.
            uint64_t exch_oid = 0;
            if (s0.at("resting").as_object().contains("oid"))
                exch_oid = s0.at("resting").as_object().at("oid").to_number<uint64_t>();
            if (exch_oid != 0 && on_acked) on_acked(exch_oid);

            ExecEvent ev = make_skeleton(ctx, now_ns);
            ev.status = ES::ACKED;
            ev.reject_reason = RR::NULL_VALUE;
            ev.exchange_order_id = exch_oid;
            ev.filled_qty = 0;
            ev.remaining_qty = ctx.quantity_e8;
            push_or_log(queue_, ev, "ACKED");
            return true;
        }

        if (s0.contains("filled")) {
            // Immediate fill-on-placement (IOC, crossing limit, or market).
            // We deliberately do NOT emit a FILLED ExecEvent here — the same
            // fill also comes through the userFills WS stream, and emitting
            // from both paths would double-count the fill in fenrir. Only
            // the WS stream is authoritative; we just register the oid so
            // the parser can resolve userFills → client order_id (HL does
            // not echo a cloid).
            const auto& f = s0.at("filled").as_object();
            const uint64_t exch_oid = f.contains("oid") ? f.at("oid").to_number<uint64_t>() : 0;
            if (exch_oid != 0 && on_acked) on_acked(exch_oid);
            bpt::common::log::debug(
                "[OrderGateway] HL emitter: fill-on-placement client_id={} exch_oid={} — waiting for userFills",
                ctx.client_order_id, exch_oid);
            return true;
        }

        if (s0.contains("error")) {
            bpt::common::log::warn("[OrderGateway] HL emitter: order error: {}",
                           std::string(s0.at("error").as_string()));
            ExecEvent ev = make_skeleton(ctx, now_ns);
            ev.status = ES::REJECTED;
            ev.reject_reason = RR::EXCHANGE_ERROR;
            ev.remaining_qty = ctx.quantity_e8;
            push_or_log(queue_, ev, "REJECTED");
            return true;
        }

        return false;
    } catch (const std::exception& e) {
        bpt::common::log::warn("[OrderGateway] HL emitter: failed to parse order resp: {} resp={}",
                       e.what(), resp);
        emit_rejected(ctx);
        return true;
    }
}

bool HyperliquidExecEmitter::emit_cancel_response(const std::string& resp,
                                                  uint64_t client_order_id,
                                                  const std::function<void()>& on_cancelled) {
    try {
        const auto rj = json::parse(resp).as_object();
        const std::string status =
            rj.contains("status") ? std::string(rj.at("status").as_string()) : "";
        if (status != "ok") return false;

        // Parse inner statuses. HL returns status:"ok" at the top level even
        // when the specific cancel failed — the per-order result is in
        // response.data.statuses[0], which is either the string "success" or
        // an object {"error":"..."}. Critically, the error string is
        //   "Order was never placed, already canceled, or filled."
        // Three different meanings squashed into one message:
        //   - never placed / already canceled → safe to treat as cancelled
        //   - already FILLED → the real fill is about to arrive via userFills.
        //     If we emit CANCELLED here and erase the oid→client mapping, the
        //     userFills event resolves to order_id=0 and fenrir loses the fill.
        // When we can't distinguish, err on the side of "wait for userFills":
        // don't emit CANCELLED, don't erase maps. Strategy's cancel-pending flag
        // will be cleared by the FILLED event moments later.
        bool ambiguous_already_filled = false;
        if (rj.contains("response") && rj.at("response").is_object()) {
            const auto& response = rj.at("response").as_object();
            if (response.contains("data") && response.at("data").is_object()) {
                const auto& data = response.at("data").as_object();
                if (data.contains("statuses") && data.at("statuses").is_array()) {
                    const auto& statuses = data.at("statuses").as_array();
                    if (!statuses.empty() && statuses[0].is_object()) {
                        const auto& s0 = statuses[0].as_object();
                        if (s0.contains("error") && s0.at("error").is_string()) {
                            const std::string_view err(s0.at("error").as_string());
                            if (err.find("filled") != std::string_view::npos) {
                                ambiguous_already_filled = true;
                                bpt::common::log::warn(
                                    "[OrderGateway] HL emitter: cancel id={} raced with fill "
                                    "(HL says '{}') — skipping CANCELLED emit, waiting for "
                                    "userFills to deliver the real state",
                                    client_order_id, err);
                            }
                        }
                    }
                }
            }
        }

        if (ambiguous_already_filled) {
            // Return true (parse succeeded) without emitting anything or
            // erasing oid maps. The authoritative FILLED event is on its way.
            return true;
        }

        const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();

        ExecEvent ev{};
        ev.order_id = client_order_id;
        ev.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
        ev.status = ES::CANCELLED;
        ev.reject_reason = RR::NULL_VALUE;
        ev.fee_currency = FC::USDT;
        ev.exchange_ts_ns = now_ns;
        ev.local_ts_ns = now_ns;
        push_or_log(queue_, ev, "CANCELLED");
        if (on_cancelled) on_cancelled();
        return true;
    } catch (const std::exception& e) {
        bpt::common::log::warn("[OrderGateway] HL emitter: failed to parse cancel resp: {}", e.what());
        return false;
    }
}

void HyperliquidExecEmitter::emit_rejected(const OrderContext& ctx) {
    const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
    ExecEvent ev = make_skeleton(ctx, now_ns);
    ev.status = ES::REJECTED;
    ev.reject_reason = RR::EXCHANGE_ERROR;
    ev.remaining_qty = ctx.quantity_e8;
    push_or_log(queue_, ev, "REJECTED");
}

void HyperliquidExecEmitter::emit_recovered_ack(const OrderContext& ctx, uint64_t exch_oid) {
    const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
    ExecEvent ev = make_skeleton(ctx, now_ns);
    ev.status = ES::ACKED;
    ev.reject_reason = RR::NULL_VALUE;
    ev.exchange_order_id = exch_oid;
    ev.filled_qty = 0;
    ev.remaining_qty = ctx.quantity_e8;
    push_or_log(queue_, ev, "RECOVERED_ACKED");
}

void HyperliquidExecEmitter::emit_recovered_fill(const OrderContext& ctx,
                                                 uint64_t exch_oid,
                                                 int64_t  fill_price_e8,
                                                 int64_t  fill_fee_e8,
                                                 uint64_t fill_qty_e8,
                                                 uint64_t fill_time_ns) {
    const uint64_t now_ns = bpt::common::util::TscClock::now_epoch_ns();
    ExecEvent ev = make_skeleton(ctx, now_ns);
    // Overwrite price with the actual fill price (may differ from the
    // intended limit price by one tick due to HL's price rounding).
    ev.price = fill_price_e8;
    ev.exchange_order_id = exch_oid;
    ev.filled_qty = fill_qty_e8;
    // If the recovered fill covered the full intent, emit FILLED. If it
    // was partial (e.g. an IOC that hit one slice), emit PARTIAL so the
    // strategy doesn't close out its resting state prematurely — any
    // trailing slices will arrive on the now-registered oid via userFills.
    const bool full = fill_qty_e8 >= ctx.quantity_e8;
    ev.status = full ? ES::FILLED : ES::PARTIAL;
    ev.remaining_qty = full ? 0 : (ctx.quantity_e8 - fill_qty_e8);
    ev.fee = fill_fee_e8;
    ev.exchange_ts_ns = fill_time_ns != 0 ? fill_time_ns : now_ns;
    ev.reject_reason = RR::OK;
    push_or_log(queue_, ev, full ? "RECOVERED_FILLED" : "RECOVERED_PARTIAL");
}

}  // namespace bpt::order_gateway::adapter::hyperliquid

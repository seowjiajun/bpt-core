#include "heimdall/adapter/hyperliquid/hyperliquid_exec_emitter.h"

#include <bifrost_protocol/ExchangeId.h>
#include <bifrost_protocol/ExecStatus.h>
#include <bifrost_protocol/FeeCurrency.h>
#include <bifrost_protocol/RejectReason.h>

#include <boost/json.hpp>
#include <cmath>
#include <yggdrasil/logging.h>
#include <yggdrasil/util/tsc_clock.h>

namespace heimdall::adapter::hyperliquid {

namespace json = boost::json;
using ES = bifrost::protocol::ExecStatus;
using RR = bifrost::protocol::RejectReason;
using FC = bifrost::protocol::FeeCurrency;

namespace {
constexpr double kScale = 1e8;

// Build the skeleton of an ExecEvent with everything the OrderContext
// knows. Callers fill in status + exch_oid + filled_qty + remaining_qty
// and push.
ExecEvent make_skeleton(const OrderContext& ctx, uint64_t now_ns) {
    ExecEvent ev{};
    ev.order_id      = ctx.client_order_id;
    ev.exchange_id   = bifrost::protocol::ExchangeId::HYPERLIQUID;
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
        ygg::log::error("[Hyperliquid] exec_queue full — dropped {} ExecEvent", tag);
}

}  // namespace

bool HyperliquidExecEmitter::emit_order_response(const std::string& resp,
                                                 const OrderContext& ctx,
                                                 const std::function<void(uint64_t)>& on_acked) {
    try {
        const auto rj = json::parse(resp).as_object();
        const std::string status =
            rj.contains("status") ? std::string(rj.at("status").as_string()) : "";

        const uint64_t now_ns = ygg::util::TscClock::now_epoch_ns();

        if (status != "ok") {
            ygg::log::warn("[Heimdall] HL emitter: order rejected, status={}", status);
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
            // Immediate fill (IOC or aggressive limit).
            const auto& f = s0.at("filled").as_object();
            const uint64_t exch_oid = f.contains("oid") ? f.at("oid").to_number<uint64_t>() : 0;
            const double total_sz = f.contains("totalSz") ? std::stod(std::string(f.at("totalSz").as_string())) : 0.0;
            const uint64_t filled_qty = static_cast<uint64_t>(std::round(total_sz * kScale));

            ExecEvent ev = make_skeleton(ctx, now_ns);
            ev.status = ES::FILLED;
            ev.reject_reason = RR::NULL_VALUE;
            ev.exchange_order_id = exch_oid;
            ev.filled_qty = filled_qty;
            ev.remaining_qty = (ctx.quantity_e8 > filled_qty) ? (ctx.quantity_e8 - filled_qty) : 0;
            push_or_log(queue_, ev, "FILLED");
            return true;
        }

        if (s0.contains("error")) {
            ygg::log::warn("[Heimdall] HL emitter: order error: {}",
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
        ygg::log::warn("[Heimdall] HL emitter: failed to parse order resp: {} resp={}",
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

        const uint64_t now_ns = ygg::util::TscClock::now_epoch_ns();

        ExecEvent ev{};
        ev.order_id = client_order_id;
        ev.exchange_id = bifrost::protocol::ExchangeId::HYPERLIQUID;
        ev.status = ES::CANCELLED;
        ev.reject_reason = RR::NULL_VALUE;
        ev.fee_currency = FC::USDT;
        ev.exchange_ts_ns = now_ns;
        ev.local_ts_ns = now_ns;
        push_or_log(queue_, ev, "CANCELLED");
        if (on_cancelled) on_cancelled();
        return true;
    } catch (const std::exception& e) {
        ygg::log::warn("[Heimdall] HL emitter: failed to parse cancel resp: {}", e.what());
        return false;
    }
}

void HyperliquidExecEmitter::emit_rejected(const OrderContext& ctx) {
    const uint64_t now_ns = ygg::util::TscClock::now_epoch_ns();
    ExecEvent ev = make_skeleton(ctx, now_ns);
    ev.status = ES::REJECTED;
    ev.reject_reason = RR::EXCHANGE_ERROR;
    ev.remaining_qty = ctx.quantity_e8;
    push_or_log(queue_, ev, "REJECTED");
}

}  // namespace heimdall::adapter::hyperliquid

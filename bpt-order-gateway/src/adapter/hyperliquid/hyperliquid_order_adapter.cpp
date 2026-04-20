#include "order_gateway/adapter/hyperliquid/hyperliquid_order_adapter.h"

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_action_codec.h"

#include <messages/ExchangeId.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>
#include <messages/TimeInForce.h>

#include <boost/json.hpp>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>

namespace bpt::order_gateway::adapter {

namespace json = boost::json;

static constexpr double kScale = 1e8;

// Shorthand for the helper classes that used to live inline in this file.
namespace hlcodec = bpt::order_gateway::adapter::hyperliquid;

HyperliquidOrderAdapter::HyperliquidOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds)
    : OrderAdapterBase(cfg),
      wallet_address_(creds.wallet_address) {
    https_client_ = std::make_unique<hyperliquid::HyperliquidHttpsClient>(cfg.rest_host, cfg.rest_port);
    ws_client_ = std::make_unique<hyperliquid::HyperliquidWsClient>(
        ioc_, ssl_ctx_, cfg.ws_host, cfg.ws_port, cfg.ws_path, wallet_address_,
        cfg.pinned_tls_sha256);

    parser_.on_exec_event = [this](const ExecEvent& ev) {
        if (!exec_queue_.try_push(ev))
            bpt::common::log::error("[Hyperliquid] exec_queue full — dropped ExecEvent order_id={}", ev.order_id);
    };

    // Route userFills frames from the ws read loop into our exec parser.
    ws_client_->set_user_fills_handler(
        [this](const boost::json::array& fills, uint64_t recv_ns) {
            parser_.handle_fills(fills, recv_ns);
        });

    if (creds.private_key.empty()) {
        enabled_ = false;
        bpt::common::log::warn("HyperliquidOrderAdapter: disabled — private_key not set");
        return;
    }
    try {
        signer_ = std::make_unique<HyperliquidSigner>(creds.private_key, !cfg.testnet);
        enabled_ = true;
        bpt::common::log::info("HyperliquidOrderAdapter: signer loaded");
    } catch (const std::exception& e) {
        enabled_ = false;
        bpt::common::log::warn("HyperliquidOrderAdapter: disabled — {}", e.what());
    }

    // Phantom-fill reconciler. Poller wraps the existing HTTPS client —
    // both /info calls are unsigned reads so no signer coupling needed.
    // price_tick_e8 = 1 USD (BTC/ETH tick is $0.1–$1 on HL; 1 USD covers
    // all current assets and leaves room for HL's post-submit rounding).
    reconciler_ = std::make_unique<hyperliquid::HyperliquidReconciler>(
        [this]() -> std::pair<json::array, json::array> {
            std::pair<json::array, json::array> out;
            try {
                json::object req_open;
                req_open["type"] = "openOrders";
                req_open["user"] = wallet_address_;
                auto open_resp = json::parse(
                    https_client_->post("/info", json::serialize(json::value(req_open))));
                if (open_resp.is_array()) out.first = open_resp.as_array();
            } catch (const std::exception& e) {
                bpt::common::log::warn("reconciler: openOrders poll failed: {}", e.what());
            }
            try {
                json::object req_fills;
                req_fills["type"] = "userFills";
                req_fills["user"] = wallet_address_;
                auto fills_resp = json::parse(
                    https_client_->post("/info", json::serialize(json::value(req_fills))));
                if (fills_resp.is_array()) out.second = fills_resp.as_array();
            } catch (const std::exception& e) {
                bpt::common::log::warn("reconciler: userFills poll failed: {}", e.what());
            }
            return out;
        },
        [this](const hyperliquid::HyperliquidReconciler::Candidate& c,
               const hyperliquid::HyperliquidReconciler::MatchResult& r) {
            on_reconcile_terminal(c, r);
        },
        std::chrono::milliseconds(3000),
        static_cast<int64_t>(1.0 * 1e8));  // 1 USD tick
}

// HTTPS/REST path extracted to HyperliquidHttpsClient.
// WebSocket + post-action plumbing extracted to HyperliquidWsClient.
// See hyperliquid_https_client.{h,cpp} and hyperliquid_ws_client.{h,cpp}.

void HyperliquidOrderAdapter::connect_and_run() {
    if (!enabled_) {
        bpt::common::log::warn("HyperliquidOrderAdapter: running in disabled mode");
        while (!stop_flag_.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::seconds(1));
        return;
    }
    ws_client_->run(stop_flag_, connected_);
}

void HyperliquidOrderAdapter::send_new_order(const bpt::messages::NewOrder& order) {
    if (!enabled_ || !signer_) {
        bpt::common::log::error("HyperliquidOrderAdapter: disabled, cannot send order");
        return;
    }

    using OS = bpt::messages::OrderSide;
    using OT = bpt::messages::OrderType;
    using TIF = bpt::messages::TimeInForce;

    const std::string exchange_symbol = order.getExchangeSymbolAsString();
    const bool is_buy = (order.side() == OS::BUY);
    const double price_d = static_cast<double>(order.price()) / kScale;
    const double size_d  = static_cast<double>(order.quantity()) / kScale;

    // Map (OrderType, TimeInForce) onto HL's limit tif string.
    //   POST_ONLY → Alo  (HL rejects the order if it would cross — maker only)
    //   LIMIT + IOC → Ioc
    //   MARKET → Ioc (HL has no market type; strategies send an aggressive
    //                 limit price with IOC semantics)
    //   everything else → Gtc
    const auto hl_tif = [&]() {
        if (order.orderType() == OT::POST_ONLY) return hlcodec::HlTif::Alo;
        if (order.timeInForce() == TIF::IOC || order.orderType() == OT::MARKET)
            return hlcodec::HlTif::Ioc;
        return hlcodec::HlTif::Gtc;
    }();

    try {
        // Build Hyperliquid order action JSON via the pure codec helper.
        // Do NOT mutate `action` after signing — the signer msgpacks the
        // exact bytes we pass to ws_client_->post_action, so any post-sign mutation
        // desyncs the signature from the wire payload.
        const json::value action = hlcodec::build_order_action(
            exchange_symbol, is_buy, price_d, size_d, hl_tif);

        const uint64_t nonce = signer_->next_nonce();
        auto tx = signer_->sign_l1_action(action, nonce);

        // Post over the existing order-gateway WS (shared with userFills sub)
        // instead of opening a fresh HTTPS /exchange request. Response
        // payload shape is identical to the REST body, so downstream
        // parsing below is unchanged.
        const std::string resp = ws_client_->post_action(action, nonce, tx);
        bpt::common::log::info("HyperliquidOrderAdapter: new order id={} side={} tif={} px={} sz={} resp={}",
                       order.orderId(),
                       is_buy ? "BUY" : "SELL",
                       hlcodec::tif_to_string(hl_tif),
                       price_d,
                       size_d,
                       resp);

        const hyperliquid::OrderContext ctx{
            order.orderId(),
            order.instrumentId(),
            order.side(),
            order.orderType(),
            order.price(),
            order.quantity(),
        };
        exec_emitter_.emit_order_response(
            resp, ctx,
            [this, client_id = order.orderId(), qty_e8 = order.quantity()](uint64_t exch_oid) {
                client_to_exch_oid_[client_id] = exch_oid;
                exch_oid_to_client_[exch_oid] = client_id;
                // Register with the parser so userFills entries for this oid
                // can be resolved to a client_order_id AND tracked across
                // multiple partial-fill slices.
                parser_.register_order(exch_oid, client_id, qty_e8);
            });
    } catch (const std::exception& e) {
        // post_action threw: either the WS was already disconnected,
        // the connection dropped mid-flight (PendingGuard in ws_client
        // fails every in-flight future), or the 5 s timeout elapsed.
        // All three cases share the same blind spot: HL may have
        // accepted the signed action even though no response reached us.
        // Instead of emitting REJECTED and letting the strategy's view
        // diverge, defer to the reconciler — it waits 3 s then REST-
        // polls /info openOrders + /info userFills to find the truth.
        bpt::common::log::warn(
            "HyperliquidOrderAdapter: send_new_order id={} threw "
            "({}) — deferring to reconciler",
            order.orderId(), e.what());
        reconciler_->reconcile_async(hyperliquid::HyperliquidReconciler::Candidate{
            order.orderId(),
            order.instrumentId(),
            order.side(),
            order.orderType(),
            order.price(),
            order.quantity(),
            exchange_symbol,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                                      .count()),
        });
    }
}

void HyperliquidOrderAdapter::on_reconcile_terminal(
    const hyperliquid::HyperliquidReconciler::Candidate& c,
    const hyperliquid::HyperliquidReconciler::MatchResult& r) {
    // Runs on the reconciler's worker thread. By this point at least
    // the grace period (default 3 s) has elapsed since the original
    // send_new_order failed, so the OrderProcessor thread is long
    // since out of the catch block. The oid maps and parser's pending_
    // are written here — strictly separated in time from the normal
    // ACK path, so the race is theoretical under current timings.

    // Race guard: if the original response arrived late (after the
    // 5 s post_action timeout but before reconciliation fired) and
    // already populated the mapping, skip the duplicate emit.
    if (client_to_exch_oid_.find(c.client_order_id) != client_to_exch_oid_.end()) {
        bpt::common::log::info(
            "HL reconciler: client_id={} already has exch_oid={} — "
            "late response beat us; skipping recovery emit",
            c.client_order_id, client_to_exch_oid_[c.client_order_id]);
        return;
    }

    const hyperliquid::OrderContext ctx{
        c.client_order_id, c.instrument_id, c.side, c.order_type,
        c.price_e8, c.quantity_e8,
    };

    using MK = hyperliquid::HyperliquidReconciler::MatchKind;
    switch (r.kind) {
        case MK::OpenOrder:
            bpt::common::log::warn(
                "HL reconciler: RECOVERED ACK client_id={} exch_oid={} "
                "(order rested on HL despite lost response)",
                c.client_order_id, r.exch_oid);
            client_to_exch_oid_[c.client_order_id] = r.exch_oid;
            exch_oid_to_client_[r.exch_oid] = c.client_order_id;
            parser_.register_order(r.exch_oid, c.client_order_id, c.quantity_e8);
            exec_emitter_.emit_recovered_ack(ctx, r.exch_oid);
            return;
        case MK::UserFill:
            bpt::common::log::warn(
                "HL reconciler: RECOVERED FILL client_id={} exch_oid={} "
                "qty_e8={} px_e8={} (phantom fill — order matched on HL despite lost response)",
                c.client_order_id, r.exch_oid, r.fill_qty_e8, r.fill_price_e8);
            client_to_exch_oid_[c.client_order_id] = r.exch_oid;
            exch_oid_to_client_[r.exch_oid] = c.client_order_id;
            // Register with the original quantity — the parser needs to
            // know the original intent so any trailing partial slices
            // that arrive later via WS userFills resolve to FILLED at
            // the right cumulative total.
            parser_.register_order(r.exch_oid, c.client_order_id, c.quantity_e8);
            exec_emitter_.emit_recovered_fill(ctx, r.exch_oid, r.fill_price_e8,
                                              r.fill_fee_e8, r.fill_qty_e8, r.fill_time_ns);
            return;
        case MK::None:
        case MK::Ambiguous:
            // Either no match (genuine reject) or multiple candidates
            // matched the same fill (can't disambiguate — safer to
            // REJECT both intents and let the strategy-side position
            // reconciler flag any divergence). Log line for Ambiguous
            // already emitted in the reconciler worker.
            exec_emitter_.emit_rejected(ctx);
            return;
    }
}

void HyperliquidOrderAdapter::send_cancel(const bpt::messages::CancelOrder& cancel,
                                          const std::string& native_symbol) {
    if (!enabled_ || !signer_) {
        bpt::common::log::error("HyperliquidOrderAdapter: disabled, cannot cancel order");
        return;
    }

    // HL's cancel-by-oid requires the EXCHANGE oid from the "resting"
    // response, not our client order_id. Look it up in the map that
    // send_new_order populated when it received the ACK.
    auto it = client_to_exch_oid_.find(cancel.orderId());
    if (it == client_to_exch_oid_.end()) {
        bpt::common::log::warn("HyperliquidOrderAdapter: cancel id={}: no exch_oid mapping — order never ACKed or already terminal",
                       cancel.orderId());
        return;
    }
    const uint64_t exch_oid = it->second;

    try {
        const json::value action = hlcodec::build_cancel_action(native_symbol, exch_oid);

        const uint64_t nonce = signer_->next_nonce();
        auto tx = signer_->sign_l1_action(action, nonce);

        const std::string resp = ws_client_->post_action(action, nonce, tx);
        bpt::common::log::info("HyperliquidOrderAdapter: cancel id={} resp={}",
                       cancel.orderId(), resp);

        exec_emitter_.emit_cancel_response(
            resp, cancel.orderId(),
            [this, client_id = cancel.orderId(), exch_oid]() {
                client_to_exch_oid_.erase(client_id);
                exch_oid_to_client_.erase(exch_oid);
            });
    } catch (const std::exception& e) {
        bpt::common::log::error("HyperliquidOrderAdapter: send_cancel failed: {}", e.what());
    }
}

void HyperliquidOrderAdapter::send_cancel_all(uint64_t instrument_id) {
    // HL doesn't expose a native per-instrument cancel-all. We implement
    // this as: fetch the wallet's open orders via /info, build a batch
    // cancel action covering every returned oid, sign, POST /exchange.
    // The instrument_id parameter is logged but ignored — HL identifies
    // orders by (asset_idx, oid) and every returned oid is cancelled.
    // This matches the Python scripts/flatten_hl_positions.py flow and
    // works regardless of scheduleCancel volume gating.
    if (!enabled_ || !signer_) {
        bpt::common::log::error("HyperliquidOrderAdapter: disabled, cannot cancel_all");
        return;
    }

    bpt::common::log::warn("HyperliquidOrderAdapter: send_cancel_all(instrument_id={}) "
                   "— cancelling ALL open orders on wallet (HL bulk cancel is not per-instrument)",
                   instrument_id);

    try {
        json::object info_req;
        info_req["type"] = "openOrders";
        info_req["user"] = wallet_address_;
        const std::string info_resp =
            https_client_->post("/info", json::serialize(json::value(info_req)));

        auto parsed = json::parse(info_resp);
        if (!parsed.is_array()) {
            bpt::common::log::warn("cancel_all: unexpected openOrders response: {}", info_resp);
            return;
        }
        const auto& orders = parsed.as_array();
        if (orders.empty()) {
            bpt::common::log::info("cancel_all: no open orders to cancel");
            return;
        }

        json::array cancels;
        for (const auto& order_val : orders) {
            if (!order_val.is_object())
                continue;
            const auto& o = order_val.as_object();
            if (!o.contains("coin") || !o.contains("oid"))
                continue;
            const std::string coin = std::string(o.at("coin").as_string());
            const uint64_t oid = static_cast<uint64_t>(o.at("oid").as_int64());

            const auto meta = hlcodec::lookup_testnet_asset(coin);
            json::object c;
            c["a"] = meta.asset_idx;
            c["o"] = oid;
            cancels.push_back(std::move(c));
        }
        if (cancels.empty()) {
            bpt::common::log::info("cancel_all: no cancellable orders after parse");
            return;
        }

        json::object action;
        action["type"] = "cancel";
        action["cancels"] = std::move(cancels);
        const json::value action_val = action;

        const uint64_t nonce = signer_->next_nonce();
        auto tx = signer_->sign_l1_action(action_val, nonce);

        json::object req;
        req["action"] = action;
        req["nonce"] = nonce;
        json::object sig;
        sig["r"] = "0x" + tx.r;
        sig["s"] = "0x" + tx.s;
        sig["v"] = tx.v;
        req["signature"] = std::move(sig);

        const std::string cancel_resp =
            https_client_->post("/exchange", json::serialize(req));
        bpt::common::log::info("cancel_all: submitted batch cancel for {} order(s), resp={}",
                       orders.size(), cancel_resp);
    } catch (const std::exception& e) {
        bpt::common::log::error("send_cancel_all failed: {}", e.what());
    }
}

void HyperliquidOrderAdapter::send_modify(const bpt::messages::ModifyOrder& modify,
                                          const std::string& native_symbol) {
    if (!enabled_ || !signer_) {
        bpt::common::log::error("HyperliquidOrderAdapter: disabled, cannot modify order");
        return;
    }

    try {
        const double price_d = static_cast<double>(modify.newPrice()) / kScale;
        const double size_d  = static_cast<double>(modify.newQuantity()) / kScale;

        const json::value action = hlcodec::build_modify_action(
            native_symbol, modify.orderId(), price_d, size_d);

        const uint64_t nonce = signer_->next_nonce();
        auto tx = signer_->sign_l1_action(action, nonce);

        // HL doesn't accept the `modify` action over the WS post
        // endpoint — it returns `{"channel":"error","data":"Error parsing
        // JSON into valid websocket request: ..."}` at parse time.
        // Fall back to REST for modify only; order/cancel still go via WS.
        json::object req;
        req["action"] = action.as_object();
        req["nonce"] = nonce;
        json::object signature;
        signature["r"] = "0x" + tx.r;
        signature["s"] = "0x" + tx.s;
        signature["v"] = tx.v;
        req["signature"] = std::move(signature);

        const std::string resp = https_client_->post("/exchange", json::serialize(req));
        bpt::common::log::debug("HyperliquidOrderAdapter: modify resp={}", resp);
    } catch (const std::exception& e) {
        bpt::common::log::error("HyperliquidOrderAdapter: send_modify failed: {}", e.what());
    }
}

AccountSnapshotData HyperliquidOrderAdapter::fetch_account_snapshot(uint64_t correlation_id) {
    const uint64_t ts_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());

    AccountSnapshotData snap;
    snap.exchange_id = bpt::messages::ExchangeId::HYPERLIQUID;
    snap.correlation_id = correlation_id;
    snap.timestamp_ns = ts_ns;

    if (!enabled_) {
        bpt::common::log::warn("HyperliquidOrderAdapter: disabled — returning empty snapshot");
        return snap;
    }

    // Wallet address required for clearinghouseState query.
    if (wallet_address_.empty()) {
        bpt::common::log::warn(
            "HyperliquidOrderAdapter: wallet_address not set — "
            "returning empty account snapshot");
        return snap;
    }
    const std::string& wallet_address = wallet_address_;

    // POST /info {type: clearinghouseState, user: <address>} — public, no signing.
    json::object req_body;
    req_body["type"] = "clearinghouseState";
    req_body["user"] = wallet_address;
    std::string resp = https_client_->post("/info", json::serialize(json::value(req_body)));

    try {
        auto j = json::parse(resp).as_object();

        // Available and total balance from marginSummary.
        if (j.contains("marginSummary") && j.at("marginSummary").is_object()) {
            const auto& ms = j.at("marginSummary").as_object();
            if (ms.contains("accountValue"))
                snap.total_equity_e8 =
                    static_cast<int64_t>(std::round(std::stod(std::string(ms.at("accountValue").as_string())) * 1e8));
        }
        if (j.contains("withdrawable"))
            snap.available_balance_e8 =
                static_cast<int64_t>(std::round(std::stod(std::string(j.at("withdrawable").as_string())) * 1e8));

        // Positions from assetPositions.
        if (j.contains("assetPositions") && j.at("assetPositions").is_array()) {
            for (const auto& ap_val : j.at("assetPositions").as_array()) {
                if (!ap_val.is_object())
                    continue;
                const auto& ap = ap_val.as_object();
                if (!ap.contains("position") || !ap.at("position").is_object())
                    continue;
                const auto& pos = ap.at("position").as_object();
                if (!pos.contains("szi"))
                    continue;
                const double szi = std::stod(std::string(pos.at("szi").as_string()));
                if (szi == 0.0)
                    continue;

                AccountPosition p;
                if (pos.contains("coin"))
                    p.exchange_symbol = std::string(pos.at("coin").as_string());
                p.net_qty_e8 = static_cast<int64_t>(std::round(szi * 1e8));
                if (pos.contains("entryPx") && pos.at("entryPx").is_string())
                    p.avg_entry_price_e8 =
                        static_cast<int64_t>(std::round(std::stod(std::string(pos.at("entryPx").as_string())) * 1e8));
                if (pos.contains("unrealizedPnl") && pos.at("unrealizedPnl").is_string())
                    p.unrealized_pnl_e8 = static_cast<int64_t>(
                        std::round(std::stod(std::string(pos.at("unrealizedPnl").as_string())) * 1e8));
                snap.positions.push_back(std::move(p));
            }
        }
    } catch (const std::exception& e) {
        bpt::common::log::error("HyperliquidOrderAdapter: failed to parse account snapshot: {}", e.what());
    }

    bpt::common::log::info(
        "HyperliquidOrderAdapter: account snapshot fetched — balance={:.2f} "
        "positions={}",
        static_cast<double>(snap.available_balance_e8) / 1e8,
        snap.positions.size());
    return snap;
}

}  // namespace bpt::order_gateway::adapter

#pragma once

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/order_adapter_base.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_exec_emitter.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_exec_parser.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_https_client.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_signer.h"
#include "order_gateway/adapter/hyperliquid/hyperliquid_ws_client.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace bpt::order_gateway::adapter {

// HyperliquidOrderAdapter sends orders to the Hyperliquid L1 via signed REST
// calls. Fill events are received from the Hyperliquid WebSocket
// (wss://api.hyperliquid.xyz/ws).
//
// Private key is passed via ExchangeCredentials.private_key (64-char hex).
// If the key is empty, the adapter starts in disabled mode — connect_and_run()
// spins on stop_flag_ rather than attempting a connection.
class HyperliquidOrderAdapter : public OrderAdapterBase {
public:
    HyperliquidOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    void send_new_order(const bpt::messages::NewOrder& order) override;
    void send_cancel(const bpt::messages::CancelOrder& cancel, const std::string& native_symbol) override;
    void send_cancel_all(uint64_t instrument_id) override;
    void send_modify(const bpt::messages::ModifyOrder& modify, const std::string& native_symbol) override;

    [[nodiscard]] bpt::messages::ExchangeId::Value exchange_id() const override {
        return bpt::messages::ExchangeId::HYPERLIQUID;
    }
    [[nodiscard]] const char* exchange_name() const override { return "HYPERLIQUID"; }
    [[nodiscard]] bool is_connected() const override { return enabled_ && connected_.load(std::memory_order_relaxed); }

    AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) override;

protected:
    void connect_and_run() override;

private:
    bool enabled_{false};  // false if private_key credential is empty
    std::string wallet_address_;
    std::unique_ptr<HyperliquidSigner> signer_;
    HyperliquidExecParser parser_;
    hyperliquid::HyperliquidExecEmitter exec_emitter_{exec_queue_};
    std::unique_ptr<hyperliquid::HyperliquidWsClient> ws_client_;

    // client_order_id → HL exchange oid (from the "resting" response).
    // HL's cancel-by-oid wants the EXCHANGE oid, not our client id, so
    // send_cancel looks up the mapping here. Single-threaded access from
    // the OrderProcessor thread (send_new_order/send_cancel never overlap).
    std::unordered_map<uint64_t, uint64_t> client_to_exch_oid_;

    // Reverse map: HL exchange oid → client_order_id. Needed because HL
    // doesn't echo a cloid in userFills (we don't send one). When a fill
    // arrives on the WS, the parser uses this lookup to resolve the fill
    // back to the internal order_id. Without it every userFills entry is
    // silently dropped. Populated whenever a new order is acked or fills
    // on placement.
    std::unordered_map<uint64_t, uint64_t> exch_oid_to_client_;

    // REST client for /info queries (clearinghouseState, meta) and as
    // a fallback for signed actions the WS post path can't handle (modify).
    std::unique_ptr<hyperliquid::HyperliquidHttpsClient> https_client_;

    // ── Crash safety: TODO ────────────────────────────────────────────
    // An earlier version of this adapter ran a dead-man-switch thread
    // that periodically posted HL's scheduleCancel action so the exchange
    // would auto-cancel all orders if order-gateway died. That turned out to
    // be unusable on this account: HL gates scheduleCancel behind
    // $1,000,000 of lifetime traded volume (error seen on testnet:
    // "Cannot set scheduled cancel time until enough volume traded.
    // Required: $1000000. Traded: $129582.4."). Until the wallet clears
    // that threshold, there is no in-adapter crash-safety layer — fenrir's
    // graceful SIGTERM handler (fenrir_app.cpp::shutdown_flatten) covers
    // the common case, and scripts/flatten_hl_positions.py is the manual
    // kill switch. The correct long-term fix is a separate watchdog
    // process with independent exchange connectivity that monitors
    // order-gateway health and fires cancellations via the same REST path the
    // flatten script uses, bypassing the volume gate.
};

}  // namespace bpt::order_gateway::adapter

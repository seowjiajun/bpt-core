#pragma once

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/order_adapter_base.h"
#include "order_gateway/adapter/deribit/deribit_exec_decoder.h"
#include "order_gateway/adapter/deribit/deribit_ws_client.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace bpt::order_gateway::adapter {

// DeribitOrderAdapter connects to Deribit private WebSocket for order flow.
//
// WS endpoint: wss://www.deribit.com/ws/api/v2 (prod)
//              wss://test.deribit.com/ws/api/v2 (testnet)
// Authentication: JSON-RPC public/auth with client_credentials grant.
// Order placement: private/buy, private/sell via WebSocket JSON-RPC.
// Execution reports: user.orders.any.raw subscription channel.
// Heartbeat: public/set_heartbeat + respond to test_request with public/test.
//
// Transport is split across:
//   - deribit_action_encoder — pure JSON-RPC envelope builders
//   - deribit_ws_client    — WS lifecycle + thread-safe send
//
// Credentials are passed directly via ExchangeCredentials (client_id, client_secret).
class DeribitOrderAdapter : public OrderAdapterBase {
public:
    DeribitOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    void send_new_order(const bpt::messages::NewOrder& order) override;
    void send_cancel(const bpt::messages::CancelOrder& cancel, const std::string& native_symbol) override;
    void send_cancel_all(uint64_t instrument_id) override;
    void send_modify(const bpt::messages::ModifyOrder& modify, const std::string& native_symbol) override;

    [[nodiscard]] bpt::messages::ExchangeId::Value exchange_id() const override {
        return bpt::messages::ExchangeId::DERIBIT;
    }
    [[nodiscard]] const char* exchange_name() const override { return "DERIBIT"; }
    [[nodiscard]] bool is_connected() const override { return connected_.load(std::memory_order_relaxed); }

    AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) override;

protected:
    void connect_and_run() override;

private:
    void handle_message(const std::string& payload, uint64_t recv_ns);

    std::string client_id_;
    std::string client_secret_;

    std::atomic<bool> logged_in_{false};

    // 8-char hex prefix unique to this process start — prevents label collisions
    // with orders from previous sessions still live on Deribit.
    std::string session_prefix_;

    // JSON-RPC request id counter — used by adapter for auth, heartbeat, send_*.
    std::atomic<uint64_t> jsonrpc_id_{1};

    // Orders queued while waiting for auth response. Flushed in
    // handle_message when the auth reply is seen.
    mutable std::mutex pending_mu_;
    std::vector<std::string> pending_sends_;

    DeribitExecDecoder decoder_;
    deribit::DeribitWsClient ws_client_;
};

}  // namespace bpt::order_gateway::adapter

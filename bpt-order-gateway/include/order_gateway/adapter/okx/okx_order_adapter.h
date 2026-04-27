#pragma once

#include "order_gateway/adapter/common/credentials.h"
#include "order_gateway/adapter/common/order_adapter_base.h"
#include "order_gateway/adapter/okx/okx_exec_decoder.h"
#include "order_gateway/adapter/okx/okx_https_client.h"
#include "order_gateway/adapter/okx/okx_instruments_service.h"
#include "order_gateway/adapter/okx/okx_ws_client.h"

#include <atomic>
#include <string>

namespace bpt::order_gateway::adapter {

// OKXOrderAdapter connects to OKX private WebSocket endpoint for order flow.
//
// WS endpoint: wss://ws.okx.com:8443/ws/v5/private
// Authentication: login message immediately after connect.
// Order placement: {"op":"order","args":[{...}]} via WebSocket.
// Execution reports: "order" and "fills" channel events.
// Ping/pong every 15 seconds to keep connection alive.
//
// Credentials are passed directly via ExchangeCredentials (api_key, secret_key, passphrase).
class OKXOrderAdapter : public OrderAdapterBase {
public:
    OKXOrderAdapter(const config::AdapterConfig& cfg, const ExchangeCredentials& creds);

    // Fetches instIdCodes from REST before spawning the IO thread.
    void start() override;

    void send_new_order(const bpt::messages::NewOrder& order) override;
    void send_cancel(const bpt::messages::CancelOrder& cancel, const std::string& native_symbol) override;
    void send_cancel_all(uint64_t instrument_id) override;
    void send_modify(const bpt::messages::ModifyOrder& modify, const std::string& native_symbol) override;

    [[nodiscard]] bpt::messages::ExchangeId::Value exchange_id() const override {
        return bpt::messages::ExchangeId::OKX;
    }
    [[nodiscard]] const char* exchange_name() const override { return "OKX"; }
    [[nodiscard]] bool is_connected() const override { return connected_.load(std::memory_order_relaxed); }

    AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) override;

protected:
    void connect_and_run() override;

private:
    void handle_message(const std::string& payload, uint64_t recv_ns);

    // Fetch /api/v5/account/config at startup and log acctLv + perm.
    // Warns prominently if the account is Level 1 (spot-only) because
    // any attempt to trade derivatives will fail with OKX sCode 51155
    // ("local compliance requirements") — the account level is the
    // real cause, not geo-compliance.
    void fetch_and_log_account_config();

    std::string api_key_;
    std::string secret_key_;
    std::string passphrase_;

    std::atomic<bool> logged_in_{false};

    // 8-char hex prefix unique to this process start — prevents cloid collisions
    // with orders from previous sessions still live on OKX.
    std::string session_prefix_;

    // WS request id counter
    std::atomic<uint64_t> ws_req_id_{1};

    OKXExecDecoder decoder_;
    okx::OKXHttpsClient https_client_;
    okx::OKXInstrumentsService instruments_;
    okx::OKXWsClient ws_client_;
};

}  // namespace bpt::order_gateway::adapter

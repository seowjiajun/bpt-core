#pragma once

#include "heimdall/adapter/common/credentials.h"
#include "heimdall/adapter/common/order_adapter_base.h"
#include "heimdall/adapter/okx/okx_exec_parser.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace heimdall::adapter {

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

    void send_new_order(const bifrost::protocol::NewOrder& order) override;
    void send_cancel(const bifrost::protocol::CancelOrder& cancel, const std::string& native_symbol) override;
    void send_cancel_all(uint64_t instrument_id) override;
    void send_modify(const bifrost::protocol::ModifyOrder& modify, const std::string& native_symbol) override;

    [[nodiscard]] bifrost::protocol::ExchangeId::Value exchange_id() const override {
        return bifrost::protocol::ExchangeId::OKX;
    }
    [[nodiscard]] const char* exchange_name() const override { return "OKX"; }
    [[nodiscard]] bool is_connected() const override { return connected_.load(std::memory_order_relaxed); }

    AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) override;

protected:
    void connect_and_run() override;

private:
    void handle_message(const std::string& payload, uint64_t recv_ns);
    std::string build_login_msg() const;

    // REST helper — used at startup to fetch instIdCode per symbol.
    std::string https_request(const std::string& method,
                              const std::string& path,
                              const std::string& body = "",
                              bool signed_req = false);
    void fetch_inst_id_codes();

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

    // instId -> instIdCode (fetched from REST at startup, required by wseeapap endpoint)
    std::unordered_map<std::string, int64_t> inst_id_codes_;
    // instId -> contract size — also pushed to parser_ after fetch_inst_id_codes().
    std::unordered_map<std::string, double> contract_sizes_;
    // WS request id counter
    std::atomic<uint64_t> ws_req_id_{1};

    // Outbound send callback — nulled on disconnect and guarded by send_mu_.
    mutable std::mutex send_mu_;
    std::vector<std::string> pending_sends_;
    std::function<void(const std::string&)> ws_send_;

    OKXExecParser parser_;
};

}  // namespace heimdall::adapter

#pragma once

#include "heimdall/adapter/common/credentials.h"
#include "heimdall/adapter/common/order_adapter_base.h"
#include "heimdall/adapter/hyperliquid/hyperliquid_exec_parser.h"
#include "heimdall/adapter/hyperliquid/hyperliquid_signer.h"

#include <memory>
#include <string>
#include <unordered_map>

namespace heimdall::adapter {

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

    void send_new_order(const bifrost::protocol::NewOrder& order) override;
    void send_cancel(const bifrost::protocol::CancelOrder& cancel, const std::string& native_symbol) override;
    void send_cancel_all(uint64_t instrument_id) override;
    void send_modify(const bifrost::protocol::ModifyOrder& modify, const std::string& native_symbol) override;

    [[nodiscard]] bifrost::protocol::ExchangeId::Value exchange_id() const override {
        return bifrost::protocol::ExchangeId::HYPERLIQUID;
    }
    [[nodiscard]] const char* exchange_name() const override { return "HYPERLIQUID"; }
    [[nodiscard]] bool is_connected() const override { return enabled_ && connected_.load(std::memory_order_relaxed); }

    AccountSnapshotData fetch_account_snapshot(uint64_t correlation_id) override;

protected:
    void connect_and_run() override;

private:
    void handle_message(const std::string& payload, uint64_t recv_ns);

    // HTTPS POST to Hyperliquid REST endpoint.
    std::string https_post(const std::string& path, const std::string& body);

    bool enabled_{false};  // false if private_key credential is empty
    std::string wallet_address_;
    std::unique_ptr<HyperliquidSigner> signer_;
    HyperliquidExecParser parser_;

    // client_order_id → HL exchange oid (from the "resting" response).
    // HL's cancel-by-oid wants the EXCHANGE oid, not our client id, so
    // send_cancel looks up the mapping here. Single-threaded access from
    // the OrderProcessor thread (send_new_order/send_cancel never overlap).
    std::unordered_map<uint64_t, uint64_t> client_to_exch_oid_;
};

}  // namespace heimdall::adapter
